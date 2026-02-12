/*
** DiskANN Search — k-NN beam search implementation
**
** Derived from libSQL DiskANN implementation
** Copyright 2024 the libSQL authors
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "diskann_search.h"
#include "diskann.h"
#include "diskann_blob.h"
#include "diskann_cache.h"
#include "diskann_internal.h"
#include "diskann_node.h"
#include "diskann_sqlite.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/**************************************************************************
** VisitedSet — O(1) hash set for visited tracking (build speed optimization)
**************************************************************************/

#define VISITED_SET_EMPTY 0xFFFFFFFFFFFFFFFFULL

/* FNV-1a hash for 64-bit integers */
static uint64_t hash_rowid(uint64_t rowid) { return rowid * 0x100000001b3ULL; }

/* Round up to next power of 2 (for hash table sizing) */
static int next_power_of_2(int n) {
  if (n <= 0)
    return 1;
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

/* Initialize hash set with power-of-2 capacity */
#ifdef TESTING
void
#else
static void
#endif
visited_set_init(VisitedSet *set, int capacity) {
  assert(set);
  assert(capacity > 0 && (capacity & (capacity - 1)) == 0); /* power of 2 */

  set->capacity = capacity;
  set->count = 0;
  set->rowids =
      (uint64_t *)sqlite3_malloc64((uint64_t)capacity * sizeof(uint64_t));

  if (set->rowids) {
    for (int i = 0; i < capacity; i++) {
      set->rowids[i] = VISITED_SET_EMPTY;
    }
  }
}

/* Check if rowid is in the set (returns 1 if found, 0 otherwise) */
#ifdef TESTING
int
#else
static int
#endif
visited_set_contains(const VisitedSet *set, uint64_t rowid) {
  if (!set || !set->rowids) {
    return 0;
  }

  uint64_t hash = hash_rowid(rowid);
  int idx = (int)(hash & (uint64_t)(set->capacity - 1));

  /* Linear probe until we find rowid or empty slot */
  for (int i = 0; i < set->capacity; i++) {
    int probe = (idx + i) & (set->capacity - 1);
    if (set->rowids[probe] == VISITED_SET_EMPTY) {
      return 0; /* Not found */
    }
    if (set->rowids[probe] == rowid) {
      return 1; /* Found */
    }
  }

  return 0; /* Table full, not found */
}

/* Add rowid to the set (idempotent - safe to add same rowid twice) */
#ifdef TESTING
void
#else
static void
#endif
visited_set_add(VisitedSet *set, uint64_t rowid) {
  if (!set || !set->rowids) {
    return;
  }

  uint64_t hash = hash_rowid(rowid);
  int idx = (int)(hash & (uint64_t)(set->capacity - 1));

  /* Linear probe until we find empty slot or existing rowid */
  for (int i = 0; i < set->capacity; i++) {
    int probe = (idx + i) & (set->capacity - 1);
    if (set->rowids[probe] == VISITED_SET_EMPTY) {
      set->rowids[probe] = rowid;
      set->count++;
      return;
    }
    if (set->rowids[probe] == rowid) {
      return; /* Already in set */
    }
  }

  /* Table full - should never happen with dynamic capacity (1.3×
   * max_candidates) */
  assert(0 && "VisitedSet full - increase capacity");
}

/* Free hash set resources */
#ifdef TESTING
void
#else
static void
#endif
visited_set_deinit(VisitedSet *set) {
  if (set && set->rowids) {
    sqlite3_free(set->rowids);
    set->rowids = NULL;
    set->count = 0;
  }
}

/* Test helper exports (conditional compilation) - no-op, functions already
 * static */
/* The static functions above are exposed via the header when TESTING is defined
 */

/**************************************************************************
** Search context — static helpers
**************************************************************************/

/* Check if a node has already been visited (now uses O(1) hash set) */
static int search_ctx_is_visited(const DiskAnnSearchCtx *ctx, uint64_t rowid) {
  return visited_set_contains(&ctx->visited_set, rowid);
}

/* Check if a node is already in the candidate queue */
static int search_ctx_has_candidate(const DiskAnnSearchCtx *ctx,
                                    uint64_t rowid) {
  for (int i = 0; i < ctx->n_candidates; i++) {
    if (ctx->candidates[i]->rowid == rowid) {
      return 1;
    }
  }
  return 0;
}

/*
** Return insertion position for a candidate, or -1 if it shouldn't be added.
** Drops the unused pIndex parameter from the original libSQL version.
*/
static int search_ctx_should_add(const DiskAnnSearchCtx *ctx,
                                 float candidate_dist) {
  return distance_buffer_insert_idx(ctx->distances, ctx->n_candidates,
                                    ctx->max_candidates, candidate_dist);
}

/*
** Mark a node as visited: set visited flag, prepend to visited list,
** add to hash set, and insert into top-K results if distance qualifies.
*/
static void search_ctx_mark_visited(DiskAnnSearchCtx *ctx, DiskAnnNode *node,
                                    float distance) {
  assert(ctx->n_unvisited > 0);
  assert(node->visited == 0);

  node->visited = 1;
  ctx->n_unvisited--;

  node->next = ctx->visited_list;
  ctx->visited_list = node;

  /* Add to hash set for O(1) future lookups */
  visited_set_add(&ctx->visited_set, node->rowid);

  /* Filter gate: skip top-K insertion if filter rejects this rowid.
  ** Node is still visited (graph bridge) — only result set is filtered. */
  if (ctx->filter_fn &&
      !ctx->filter_fn((int64_t)node->rowid, ctx->filter_ctx)) {
    return;
  }

  int insert_idx =
      distance_buffer_insert_idx(ctx->top_distances, ctx->n_top_candidates,
                                 ctx->max_top_candidates, distance);
  if (insert_idx < 0) {
    return;
  }
  buffer_insert((uint8_t *)ctx->top_candidates, ctx->n_top_candidates,
                ctx->max_top_candidates, insert_idx, (int)sizeof(DiskAnnNode *),
                (const uint8_t *)&node, NULL);
  buffer_insert((uint8_t *)ctx->top_distances, ctx->n_top_candidates,
                ctx->max_top_candidates, insert_idx, (int)sizeof(float),
                (const uint8_t *)&distance, NULL);
  if (ctx->n_top_candidates < ctx->max_top_candidates) {
    ctx->n_top_candidates++;
  }
}

static int search_ctx_has_unvisited(const DiskAnnSearchCtx *ctx) {
  return ctx->n_unvisited > 0;
}

static void search_ctx_get_candidate(DiskAnnSearchCtx *ctx, int i,
                                     DiskAnnNode **node, float *distance) {
  assert(0 <= i && i < ctx->n_candidates);
  *node = ctx->candidates[i];
  *distance = ctx->distances[i];
}

/* Delete a candidate (zombie edge handling). Frees the node. */
static void search_ctx_delete_candidate(DiskAnnSearchCtx *ctx, int i) {
  assert(ctx->n_unvisited > 0);
  assert(!ctx->candidates[i]->visited);
  assert(ctx->candidates[i]->blob_spot == NULL);

  diskann_node_free(ctx->candidates[i]);
  buffer_delete((uint8_t *)ctx->candidates, ctx->n_candidates, i,
                (int)sizeof(DiskAnnNode *));
  buffer_delete((uint8_t *)ctx->distances, ctx->n_candidates, i,
                (int)sizeof(float));

  ctx->n_candidates--;
  ctx->n_unvisited--;
}

/*
** Insert a candidate at position. If the buffer is full, the furthest
** candidate is evicted (freed if unvisited).
*/
static void search_ctx_insert_candidate(DiskAnnSearchCtx *ctx, int insert_idx,
                                        DiskAnnNode *candidate,
                                        float distance) {
  DiskAnnNode *last = NULL;
  buffer_insert((uint8_t *)ctx->candidates, ctx->n_candidates,
                ctx->max_candidates, insert_idx, (int)sizeof(DiskAnnNode *),
                (const uint8_t *)&candidate, (uint8_t *)&last);
  buffer_insert((uint8_t *)ctx->distances, ctx->n_candidates,
                ctx->max_candidates, insert_idx, (int)sizeof(float),
                (const uint8_t *)&distance, NULL);

  if (ctx->n_candidates < ctx->max_candidates) {
    ctx->n_candidates++;
  }

  if (last != NULL && !last->visited) {
    assert(last->blob_spot == NULL);
    ctx->n_unvisited--;
    diskann_node_free(last);
  }
  ctx->n_unvisited++;
}

/*
** Find the closest unvisited candidate. The candidates array is sorted
** by distance, so we return the first unvisited one.
*/
static int search_ctx_find_closest_unvisited(const DiskAnnSearchCtx *ctx) {
  for (int i = 0; i < ctx->n_candidates; i++) {
    if (!ctx->candidates[i]->visited) {
      return i;
    }
  }
  return -1;
}

/**************************************************************************
** Search context — public functions
**************************************************************************/

int diskann_search_ctx_init(DiskAnnSearchCtx *ctx, const float *query,
                            int max_candidates, int max_top, int blob_mode) {
  ctx->query = query;
  ctx->n_candidates = 0;
  ctx->max_candidates = max_candidates;
  ctx->n_top_candidates = 0;
  ctx->max_top_candidates = max_top;
  ctx->visited_list = NULL;
  ctx->n_unvisited = 0;
  ctx->blob_mode = blob_mode;
  ctx->filter_fn = NULL;
  ctx->filter_ctx = NULL;

  /* Initialize hash set for O(1) visited checks
   * Capacity = next power-of-2 >= max_candidates * 1.3 (30% margin for hash
   * collisions) Minimum capacity = 256
   */
  int required_capacity = (int)((float)max_candidates * 1.3f);
  int capacity = next_power_of_2(required_capacity);
  if (capacity < 256)
    capacity = 256;
  visited_set_init(&ctx->visited_set, capacity);

  ctx->distances = (float *)sqlite3_malloc(max_candidates * (int)sizeof(float));
  ctx->candidates = (DiskAnnNode **)sqlite3_malloc(max_candidates *
                                                   (int)sizeof(DiskAnnNode *));
  ctx->top_distances = (float *)sqlite3_malloc(max_top * (int)sizeof(float));
  ctx->top_candidates =
      (DiskAnnNode **)sqlite3_malloc(max_top * (int)sizeof(DiskAnnNode *));

  if (ctx->distances && ctx->candidates && ctx->top_distances &&
      ctx->top_candidates && ctx->visited_set.rowids) {
    return DISKANN_OK;
  }

  /* Allocation failure — clean up partial allocations */
  visited_set_deinit(&ctx->visited_set);
  if (ctx->distances)
    sqlite3_free(ctx->distances);
  if (ctx->candidates)
    sqlite3_free(ctx->candidates);
  if (ctx->top_distances)
    sqlite3_free(ctx->top_distances);
  if (ctx->top_candidates)
    sqlite3_free(ctx->top_candidates);
  ctx->distances = NULL;
  ctx->candidates = NULL;
  ctx->top_distances = NULL;
  ctx->top_candidates = NULL;

  return DISKANN_ERROR_NOMEM;
}

void diskann_search_ctx_deinit(DiskAnnSearchCtx *ctx) {
  /* Free unvisited candidates still in the array (visited ones are in
   * the visited list and will be freed below) */
  for (int i = 0; i < ctx->n_candidates; i++) {
    if (!ctx->candidates[i]->visited) {
      diskann_node_free(ctx->candidates[i]);
    }
  }

  /* Free all visited nodes */
  DiskAnnNode *node = ctx->visited_list;
  while (node != NULL) {
    DiskAnnNode *next = node->next;
    diskann_node_free(node);
    node = next;
  }

  /* Free hash set */
  visited_set_deinit(&ctx->visited_set);

  sqlite3_free(ctx->candidates);
  sqlite3_free(ctx->distances);
  sqlite3_free(ctx->top_candidates);
  sqlite3_free(ctx->top_distances);
}

/**************************************************************************
** Random start node selection
**************************************************************************/

int diskann_select_random_shadow_row(const DiskAnnIndex *idx, uint64_t *rowid) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  char *sql = sqlite3_mprintf(
      "SELECT rowid FROM \"%w\".%s LIMIT 1 OFFSET ABS(RANDOM()) %% "
      "MAX((SELECT COUNT(*) FROM \"%w\".%s), 1)",
      idx->db_name, idx->shadow_name, idx->db_name, idx->shadow_name);
  if (sql == NULL) {
    return DISKANN_ERROR_NOMEM;
  }

  rc = sqlite3_prepare_v2(idx->db, sql, -1, &stmt, 0);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    return DISKANN_ERROR;
  }

  rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    /* SQLITE_DONE means empty table; propagate as-is */
    sqlite3_finalize(stmt);
    return rc;
  }

  *rowid = (uint64_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return DISKANN_OK;
}

/**************************************************************************
** Core beam search
**************************************************************************/

int diskann_search_internal(DiskAnnIndex *idx, DiskAnnSearchCtx *ctx,
                            uint64_t start_rowid, BlobCache *cache) {
  DiskAnnNode *start = NULL;
  BlobSpot *reusable_blob = NULL;
  int rc;

  start = diskann_node_alloc(start_rowid);
  if (start == NULL) {
    rc = DISKANN_ERROR_NOMEM;
    goto out;
  }

  /* Check cache for start node */
  if (cache) {
    start->blob_spot = blob_cache_get(cache, start_rowid);
  }

  if (start->blob_spot == NULL) {
    rc = blob_spot_create(idx, &start->blob_spot, start_rowid, idx->block_size,
                          ctx->blob_mode);
    if (rc != DISKANN_OK) {
      goto out;
    }

    rc = blob_spot_reload(idx, start->blob_spot, start_rowid, idx->block_size);
    if (rc != DISKANN_OK) {
      goto out;
    }

    /* Add to cache on miss */
    if (cache) {
      blob_cache_put(cache, start_rowid, start->blob_spot);
    }
  }

  const float *start_vec = node_bin_vector(idx, start->blob_spot);
  float start_distance =
      diskann_distance(ctx->query, start_vec, idx->dimensions, idx->metric);

  /* In READONLY mode, steal the blob for reuse across candidates */
  if (ctx->blob_mode == DISKANN_BLOB_READONLY) {
    assert(start->blob_spot != NULL);
    reusable_blob = start->blob_spot;
    start->blob_spot = NULL;
  }

  /* Transfer ownership of start node to the search context */
  search_ctx_insert_candidate(ctx, 0, start, start_distance);
  start = NULL;

  while (search_ctx_has_unvisited(ctx)) {
    DiskAnnNode *candidate;
    BlobSpot *candidate_blob;
    float distance;
    int i_candidate = search_ctx_find_closest_unvisited(ctx);
    search_ctx_get_candidate(ctx, i_candidate, &candidate, &distance);

    rc = DISKANN_OK;
    if (reusable_blob != NULL) {
      rc = blob_spot_reload(idx, reusable_blob, candidate->rowid,
                            idx->block_size);
      candidate_blob = reusable_blob;
    } else {
      /* Check cache first (WRITABLE mode during insert) */
      if (cache && candidate->blob_spot == NULL) {
        candidate->blob_spot = blob_cache_get(cache, candidate->rowid);
      }

      if (candidate->blob_spot == NULL) {
        rc = blob_spot_create(idx, &candidate->blob_spot, candidate->rowid,
                              idx->block_size, ctx->blob_mode);
        if (rc == DISKANN_OK) {
          rc = blob_spot_reload(idx, candidate->blob_spot, candidate->rowid,
                                idx->block_size);
        }

        /* Add to cache on miss */
        if (rc == DISKANN_OK && cache) {
          blob_cache_put(cache, candidate->rowid, candidate->blob_spot);
        }
      }
      candidate_blob = candidate->blob_spot;
    }

    if (rc == DISKANN_ROW_NOT_FOUND) {
      /* Zombie edge — deleted node. Remove candidate and continue. */
      search_ctx_delete_candidate(ctx, i_candidate);
      continue;
    } else if (rc != DISKANN_OK) {
      goto out;
    }

    /* Float32-only: no pNode != pEdge recalculation needed */
    search_ctx_mark_visited(ctx, candidate, distance);

    int n_edges = node_bin_edges(idx, candidate_blob);
    for (int i = 0; i < n_edges; i++) {
      uint64_t edge_rowid;
      const float *edge_vector;
      node_bin_edge(idx, candidate_blob, i, &edge_rowid, NULL, &edge_vector);

      if (search_ctx_is_visited(ctx, edge_rowid) ||
          search_ctx_has_candidate(ctx, edge_rowid)) {
        continue;
      }

      float edge_distance = diskann_distance(ctx->query, edge_vector,
                                             idx->dimensions, idx->metric);
      int insert_idx = search_ctx_should_add(ctx, edge_distance);
      if (insert_idx < 0) {
        continue;
      }

      DiskAnnNode *new_candidate = diskann_node_alloc(edge_rowid);
      if (new_candidate == NULL) {
        continue;
      }

      search_ctx_insert_candidate(ctx, insert_idx, new_candidate,
                                  edge_distance);
    }
  }

  rc = DISKANN_OK;

out:
  if (start != NULL) {
    diskann_node_free(start);
  }
  if (reusable_blob != NULL) {
    blob_spot_free(reusable_blob);
  }
  return rc;
}

/**************************************************************************
** Public search API
**************************************************************************/

int diskann_search(DiskAnnIndex *idx, const float *query, uint32_t dims, int k,
                   DiskAnnResult *results) {
  DiskAnnSearchCtx ctx;
  uint64_t start_rowid = 0;
  int rc;

  /* Validate inputs */
  if (!idx)
    return DISKANN_ERROR_INVALID;
  if (!query)
    return DISKANN_ERROR_INVALID;
  if (!results)
    return DISKANN_ERROR_INVALID;
  if (k < 0)
    return DISKANN_ERROR_INVALID;
  if (dims != idx->dimensions)
    return DISKANN_ERROR_DIMENSION;
  if (k == 0)
    return 0;

  /* Find a random start node */
  rc = diskann_select_random_shadow_row(idx, &start_rowid);
  if (rc == SQLITE_DONE) {
    /* Empty table — return 0 results */
    return 0;
  }
  if (rc != DISKANN_OK) {
    return DISKANN_ERROR;
  }

  /* Initialize search context */
  rc = diskann_search_ctx_init(&ctx, query, (int)idx->search_list_size, k,
                               DISKANN_BLOB_READONLY);
  if (rc != DISKANN_OK) {
    return rc;
  }
  /* Run beam search (no cache for read-only user queries) */
  rc = diskann_search_internal(idx, &ctx, start_rowid, NULL);
  if (rc != DISKANN_OK) {
    diskann_search_ctx_deinit(&ctx);
    return rc;
  }

  /* Copy top-K results to caller's array */
  int n_results = k < ctx.n_top_candidates ? k : ctx.n_top_candidates;
  for (int i = 0; i < n_results; i++) {
    results[i].id = (int64_t)ctx.top_candidates[i]->rowid;
    results[i].distance = ctx.top_distances[i];
  }

  diskann_search_ctx_deinit(&ctx);

  return n_results;
}

int diskann_search_filtered(DiskAnnIndex *idx, const float *query,
                            uint32_t dims, int k, DiskAnnResult *results,
                            DiskAnnFilterFn filter_fn, void *filter_ctx) {
  DiskAnnSearchCtx ctx;
  uint64_t start_rowid = 0;
  int rc;

  /* Same validation as diskann_search() */
  if (!idx)
    return DISKANN_ERROR_INVALID;
  if (!query)
    return DISKANN_ERROR_INVALID;
  if (!results)
    return DISKANN_ERROR_INVALID;
  if (k < 0)
    return DISKANN_ERROR_INVALID;
  if (dims != idx->dimensions)
    return DISKANN_ERROR_DIMENSION;
  if (k == 0)
    return 0;

  /* NULL filter → fall through to unfiltered search */
  if (!filter_fn) {
    return diskann_search(idx, query, dims, k, results);
  }

  /* Find a random start node */
  rc = diskann_select_random_shadow_row(idx, &start_rowid);
  if (rc == SQLITE_DONE) {
    return 0; /* Empty table */
  }
  if (rc != DISKANN_OK) {
    return DISKANN_ERROR;
  }

  /* Wider beam to compensate for filtered-out candidates */
  uint32_t beam = idx->search_list_size * 2;
  uint32_t k_scaled = (uint32_t)k * 4;
  int max_candidates = (int)(beam > k_scaled ? beam : k_scaled);

  /* Initialize search context with filter */
  rc = diskann_search_ctx_init(&ctx, query, max_candidates, k,
                               DISKANN_BLOB_READONLY);
  if (rc != DISKANN_OK) {
    return rc;
  }
  ctx.filter_fn = filter_fn;
  ctx.filter_ctx = filter_ctx;

  /* Run beam search */
  rc = diskann_search_internal(idx, &ctx, start_rowid, NULL);
  if (rc != DISKANN_OK) {
    diskann_search_ctx_deinit(&ctx);
    return rc;
  }

  /* Copy top-K results to caller's array */
  int n_results = k < ctx.n_top_candidates ? k : ctx.n_top_candidates;
  for (int i = 0; i < n_results; i++) {
    results[i].id = (int64_t)ctx.top_candidates[i]->rowid;
    results[i].distance = ctx.top_distances[i];
  }

  diskann_search_ctx_deinit(&ctx);
  return n_results;
}
