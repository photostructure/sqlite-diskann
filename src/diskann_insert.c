/*
** DiskANN Insert — vector insertion with edge pruning
**
** Derived from libSQL DiskANN implementation
** Copyright 2024 the libSQL authors
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#define _POSIX_C_SOURCE 199309L
#include "diskann.h"
#include "diskann_blob.h"
#include "diskann_cache.h"
#include "diskann_internal.h"
#include "diskann_node.h"
#include "diskann_search.h"
#include "diskann_sqlite.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/**************************************************************************
** Insert timing instrumentation
**
** Set DISKANN_DEBUG_TIMING=1 to log per-insert phase timing to stderr.
** Format is one CSV line per insert, suitable for parsing with awk/python.
** Overhead when disabled: one static variable check per insert call.
**************************************************************************/

static int insert_timing_enabled(void) {
  static int checked = 0;
  static int enabled = 0;
  if (!checked) {
    const char *env = getenv("DISKANN_DEBUG_TIMING");
    enabled = (env != NULL && env[0] != '0' && env[0] != '\0');
    if (enabled) {
      fprintf(stderr,
              "DISKANN_TIMING_HEADER: id,total_us,random_start_us,savepoint_us,"
              "search_us,shadow_row_us,phase1_us,phase2_us,flush_new_us,"
              "cleanup_us,cache_hits,cache_misses,visited_count,"
              "phase2_flushes\n");
    }
    checked = 1;
  }
  return enabled;
}

static long elapsed_us(const struct timespec *start,
                       const struct timespec *end) {
  return (long)(end->tv_sec - start->tv_sec) * 1000000L +
         (long)(end->tv_nsec - start->tv_nsec) / 1000L;
}

/**************************************************************************
** Edge replacement decision
**
** Determines where to insert a new edge in a node's edge list:
** - Returns existing index if zombie/duplicate edge found
** - Returns nEdges to append (if room)
** - Returns replacement index for worst existing edge
** - Returns -1 if new edge is dominated (skip)
**
** Float32-only: no V1 format branches, no VectorPair.
**************************************************************************/

static int replace_edge_idx(const DiskAnnIndex *idx, BlobSpot *node_blob,
                            uint64_t new_rowid, const float *new_vector,
                            float *out_distance) {
  int n_edges = (int)node_bin_edges(idx, node_blob);
  int max_edges = (int)node_edges_max_count(idx);
  int i_replace = -1;
  float node_to_replace = 0.0f;

  const float *node_vector = node_bin_vector(idx, node_blob);
  float node_to_new =
      diskann_distance(node_vector, new_vector, idx->dimensions, idx->metric);
  *out_distance = node_to_new;

  for (int i = n_edges - 1; i >= 0; i--) {
    uint64_t edge_rowid;
    float node_to_edge;
    const float *edge_vector;

    node_bin_edge(idx, node_blob, i, &edge_rowid, &node_to_edge, &edge_vector);
    if (edge_rowid == new_rowid) {
      /* Zombie or duplicate edge — replace it */
      return i;
    }

    /* No V1 branch — V3 always has stored distances */

    float edge_to_new =
        diskann_distance(edge_vector, new_vector, idx->dimensions, idx->metric);
    if (node_to_new > idx->pruning_alpha * edge_to_new) {
      /* New edge is dominated by existing edge */
      return -1;
    }
    if (node_to_new < node_to_edge &&
        (i_replace == -1 || node_to_replace < node_to_edge)) {
      node_to_replace = node_to_edge;
      i_replace = i;
    }
  }

  if (n_edges < max_edges) {
    return n_edges; /* Append */
  }
  return i_replace;
}

/**************************************************************************
** Edge pruning
**
** After inserting edge at position iInserted, remove edges that are
** dominated by the new edge. An edge E is dominated if:
**   dist(node, E) > alpha * dist(new_edge, E)
**
** This maintains graph diversity by preventing redundant edges to
** clustered nodes.
**************************************************************************/

static void prune_edges(const DiskAnnIndex *idx, BlobSpot *node_blob,
                        int i_inserted) {
  int n_edges = (int)node_bin_edges(idx, node_blob);

  assert(0 <= i_inserted && i_inserted < n_edges);

  /*
  ** Minimum degree to maintain graph connectivity
  ** Research shows MIN_DEGREE >= 8 prevents disconnected components at scale
  */
  const int MIN_DEGREE = 8;

  uint64_t hint_rowid;
  const float *hint_vector;
  node_bin_edge(idx, node_blob, i_inserted, &hint_rowid, NULL, &hint_vector);

  int i = 0;
  while (i < n_edges) {
    uint64_t edge_rowid;
    float node_to_edge;
    const float *edge_vector;

    node_bin_edge(idx, node_blob, i, &edge_rowid, &node_to_edge, &edge_vector);

    if (hint_rowid == edge_rowid) {
      i++;
      continue;
    }

    /* Stop pruning if we've reached minimum degree */
    if (n_edges <= MIN_DEGREE) {
      break;
    }

    /* No V1 branch */

    float hint_to_edge = diskann_distance(hint_vector, edge_vector,
                                          idx->dimensions, idx->metric);
    if (node_to_edge > idx->pruning_alpha * hint_to_edge) {
      node_bin_delete_edge(idx, node_blob, i);
      n_edges--;
    } else {
      i++;
    }
  }

  /* Every node needs at least one edge so the graph stays connected */
  assert(n_edges > 0);
}

/**************************************************************************
** Shadow row insertion
**************************************************************************/

static int insert_shadow_row(DiskAnnIndex *idx, int64_t id) {
  sqlite3_stmt *stmt = NULL;
  int rc;

  char *sql = sqlite3_mprintf(
      "INSERT INTO \"%w\".%s (id, data) VALUES (?, zeroblob(%d))", idx->db_name,
      idx->shadow_name, (int)idx->block_size);
  if (!sql) {
    return DISKANN_ERROR_NOMEM;
  }

  rc = sqlite3_prepare_v2(idx->db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    return DISKANN_ERROR;
  }

  sqlite3_bind_int64(stmt, 1, id);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc == SQLITE_CONSTRAINT) {
    return DISKANN_ERROR_EXISTS;
  }
  if (rc != SQLITE_DONE) {
    return DISKANN_ERROR;
  }

  return DISKANN_OK;
}

/**************************************************************************
** Public insert API
**************************************************************************/

int diskann_insert(DiskAnnIndex *idx, int64_t id, const float *vector,
                   uint32_t dims) {
  DiskAnnSearchCtx ctx = {0};
  BlobSpot *new_blob = NULL;
  BlobCache cache = {0};
  int cache_initialized = 0;
  char *savepoint_sql = NULL;
  uint64_t start_rowid = 0;
  int rc;
  int first = 0;
  int ctx_valid = 0;
  int savepoint_active = 0;

  /* Timing instrumentation (zero cost when disabled) */
  int timing = insert_timing_enabled();
  struct timespec t_entry = {0}, t_random = {0}, t_savepoint = {0};
  struct timespec t_search = {0}, t_shadow = {0}, t_phase1 = {0};
  struct timespec t_phase2 = {0}, t_flush_new = {0}, t_exit = {0};
  int phase2_flushes = 0;
  int visited_count = 0;
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_entry);
  }

  /* Validate inputs */
  if (!idx)
    return DISKANN_ERROR_INVALID;
  if (!vector)
    return DISKANN_ERROR_INVALID;
  if (dims != idx->dimensions)
    return DISKANN_ERROR_DIMENSION;

  /* Select random start node BEFORE inserting (avoids zombie confusion) */
  rc = diskann_select_random_shadow_row(idx, &start_rowid);

  if (rc == SQLITE_DONE) {
    first = 1;
  } else if (rc != DISKANN_OK) {
    return DISKANN_ERROR;
  }
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_random);
  }

  /* Begin SAVEPOINT before search — writable blob handles require an
   * active transaction, and SAVEPOINT must be started before any blob
   * opens to avoid SQLITE_BUSY.
   *
   * When called from vtab xUpdate, there's already an active SQL statement
   * so SAVEPOINT fails with SQLITE_BUSY. That's OK — the vtab's implicit
   * transaction provides atomicity for shadow table operations. */
  savepoint_sql =
      sqlite3_mprintf("SAVEPOINT diskann_insert_%s", idx->index_name);
  if (!savepoint_sql) {
    return DISKANN_ERROR_NOMEM;
  }
  rc = sqlite3_exec(idx->db, savepoint_sql, NULL, NULL, NULL);

  sqlite3_free(savepoint_sql);
  savepoint_sql = NULL;
  if (rc == SQLITE_OK) {
    savepoint_active = 1;
  }
  /* If SAVEPOINT failed (e.g., SQLITE_BUSY from vtab context), continue
   * without it — the caller's transaction provides atomicity. */
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_savepoint);
  }

  /* Search for neighbors (skip if first node) */
  if (!first) {
    /* Initialize cache (capacity 100 = ~8MB memory overhead) */
    rc = blob_cache_init(&cache, 100);
    if (rc != DISKANN_OK) {
      goto out;
    }
    cache_initialized = 1;

    rc = diskann_search_ctx_init(&ctx, vector, (int)idx->insert_list_size, 1,
                                 DISKANN_BLOB_WRITABLE);

    if (rc != DISKANN_OK) {
      goto out;
    }
    ctx_valid = 1;

    rc = diskann_search_internal(idx, &ctx, start_rowid, &cache);

    if (rc != DISKANN_OK) {
      goto out;
    }

    /* Count visited nodes for timing log */
    if (timing) {
      for (DiskAnnNode *v = ctx.visited_list; v != NULL; v = v->next) {
        visited_count++;
      }
    }
  }
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_search);
  }

  /* Insert shadow row */
  rc = insert_shadow_row(idx, id);

  if (rc != DISKANN_OK) {
    goto out;
  }

  /* Create BlobSpot for new row, load zeroblob, then initialize node */
  rc = blob_spot_create(idx, &new_blob, (uint64_t)id, idx->block_size,
                        DISKANN_BLOB_WRITABLE);

  if (rc != DISKANN_OK) {
    goto out;
  }

  rc = blob_spot_reload(idx, new_blob, (uint64_t)id, idx->block_size);

  if (rc != DISKANN_OK) {
    goto out;
  }

  node_bin_init(idx, new_blob, (uint64_t)id, vector);
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_shadow);
  }

  if (first) {
    /* First node: no edges to build, just flush and return */
    rc = blob_spot_flush(idx, new_blob);
    if (rc != DISKANN_OK) {
      goto out;
    }
    rc = DISKANN_OK;
    goto out;
  }

  /* Phase 1: add visited nodes as edges to the NEW node */
  for (DiskAnnNode *visited = ctx.visited_list; visited != NULL;
       visited = visited->next) {
    int i_replace;
    float distance;

    const float *visited_vector = node_bin_vector(idx, visited->blob_spot);

    i_replace = replace_edge_idx(idx, new_blob, visited->rowid, visited_vector,
                                 &distance);
    if (i_replace == -1) {
      continue;
    }
    node_bin_replace_edge(idx, new_blob, i_replace, visited->rowid, distance,
                          visited_vector);
    prune_edges(idx, new_blob, i_replace);
  }
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_phase1);
  }

  /* Phase 2: add NEW node as edge to visited nodes */
  for (DiskAnnNode *visited = ctx.visited_list; visited != NULL;
       visited = visited->next) {
    int i_replace;
    float distance;

    i_replace = replace_edge_idx(idx, visited->blob_spot, (uint64_t)id, vector,
                                 &distance);
    if (i_replace == -1) {
      continue;
    }
    node_bin_replace_edge(idx, visited->blob_spot, i_replace, (uint64_t)id,
                          distance, vector);
    prune_edges(idx, visited->blob_spot, i_replace);

    rc = blob_spot_flush(idx, visited->blob_spot);
    if (rc != DISKANN_OK) {
      goto out;
    }
    phase2_flushes++;
  }
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_phase2);
  }

  /* Flush new node's blob */
  rc = blob_spot_flush(idx, new_blob);
  if (rc != DISKANN_OK) {
    goto out;
  }
  if (timing) {
    clock_gettime(CLOCK_MONOTONIC, &t_flush_new);
  }

  rc = DISKANN_OK;

out:
  /* Release or rollback SAVEPOINT */
  if (savepoint_active) {
    char *sp_sql;
    if (rc == DISKANN_OK) {
      sp_sql = sqlite3_mprintf("RELEASE SAVEPOINT diskann_insert_%s",
                               idx->index_name);
    } else {
      sp_sql = sqlite3_mprintf("ROLLBACK TO SAVEPOINT diskann_insert_%s",
                               idx->index_name);
    }
    if (sp_sql) {
      sqlite3_exec(idx->db, sp_sql, NULL, NULL, NULL);
      sqlite3_free(sp_sql);
      /* After rollback, also release the savepoint */
      if (rc != DISKANN_OK) {
        char *rel_sql = sqlite3_mprintf("RELEASE SAVEPOINT diskann_insert_%s",
                                        idx->index_name);
        if (rel_sql) {
          sqlite3_exec(idx->db, rel_sql, NULL, NULL, NULL);
          sqlite3_free(rel_sql);
        }
      }
    }
  }

  if (new_blob) {
    blob_spot_free(new_blob);
  }
  if (cache_initialized) {
    blob_cache_deinit(&cache);
  }
  if (ctx_valid) {
    diskann_search_ctx_deinit(&ctx);
  }

  /* Emit timing log line (only on success for non-first inserts) */
  if (timing && !first && rc == DISKANN_OK) {
    clock_gettime(CLOCK_MONOTONIC, &t_exit);
    fprintf(
        stderr,
        "DISKANN_TIMING: %lld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,"
        "%d,%d,%d,%d\n",
        (long long)id, elapsed_us(&t_entry, &t_exit),
        elapsed_us(&t_entry, &t_random), elapsed_us(&t_random, &t_savepoint),
        elapsed_us(&t_savepoint, &t_search), elapsed_us(&t_search, &t_shadow),
        elapsed_us(&t_shadow, &t_phase1), elapsed_us(&t_phase1, &t_phase2),
        elapsed_us(&t_phase2, &t_flush_new), elapsed_us(&t_flush_new, &t_exit),
        cache.hits, cache.misses, visited_count, phase2_flushes);
  }

  return rc;
}
