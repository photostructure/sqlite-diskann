/*
** Integration tests for DiskANN — end-to-end workflows
**
** These tests exercise the full public API working together,
** covering gaps not addressed by per-module unit tests:
**
** 1. REOPEN PERSISTENCE — close and reopen index, verify data survives
** 2. CLEAR THEN REINSERT — clear wipes vectors, reinsertion works
** 3. HIGHER-DIM RECALL — 200 vectors at 128D, brute-force comparison
** 4. DELETE AT SCALE — insert 50, delete 10, verify search quality
**
** All tests use 128D vectors (realistic for embeddings) with seeded
** random generation for reproducibility.
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "unity/unity.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/diskann.h"
#include "../../src/diskann_node.h"

/*
** 128D config: block_size=16384 gives max_edges=30, plenty for
*max_neighbors=16.
** Layout: node_overhead = 16 + 512 = 528, edge_overhead = 512 + 16 = 528.
** max_edges = (16384 - 528) / 528 = 30.
*/
#define INTEG_DIMS 128
#define INTEG_BLOCK_SIZE 16384
#define INTEG_MAX_NEIGHBORS 16
#define INTEG_SEARCH_L 64
#define INTEG_INSERT_L 128

/**************************************************************************
** Helpers
**************************************************************************/

static sqlite3 *open_db(void) {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  return db;
}

static DiskAnnConfig integ_config(void) {
  DiskAnnConfig cfg = {.dimensions = INTEG_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = INTEG_MAX_NEIGHBORS,
                       .search_list_size = INTEG_SEARCH_L,
                       .insert_list_size = INTEG_INSERT_L,
                       .block_size = INTEG_BLOCK_SIZE};
  return cfg;
}

static DiskAnnIndex *create_and_open(sqlite3 *db, const char *name,
                                     const DiskAnnConfig *config) {
  int rc = diskann_create_index(db, "main", name, config);
  if (rc != DISKANN_OK)
    return NULL;

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", name, &idx);
  if (rc != DISKANN_OK)
    return NULL;

  return idx;
}

/*
** Deterministic pseudo-random float in [0, 1) using a simple LCG.
** The seed pointer is updated in place for sequential generation.
*/
static float rand_float(uint32_t *seed) {
  *seed = (*seed) * 1103515245u + 12345u;
  return (float)(*seed & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}

/*
** Generate n_vectors of INTEG_DIMS-dimensional random vectors.
** Caller must free the returned pointer.
*/
static float *gen_vectors(int n_vectors, uint32_t seed) {
  float *vecs = (float *)malloc((size_t)n_vectors * INTEG_DIMS * sizeof(float));
  TEST_ASSERT_NOT_NULL(vecs);
  for (int i = 0; i < n_vectors * INTEG_DIMS; i++) {
    vecs[i] = rand_float(&seed);
  }
  return vecs;
}

/*
** Brute-force k-NN: compute distances from query to all vectors,
** return the top-k IDs (1-based) and distances. Simple selection sort.
*/
static void brute_force_knn(const float *vectors, int n_vectors,
                            const float *query, int k, int64_t *out_ids,
                            float *out_distances) {
  float *dists = (float *)malloc((size_t)n_vectors * sizeof(float));
  int *indices = (int *)malloc((size_t)n_vectors * sizeof(int));
  TEST_ASSERT_NOT_NULL(dists);
  TEST_ASSERT_NOT_NULL(indices);

  for (int i = 0; i < n_vectors; i++) {
    dists[i] = diskann_distance_l2(query, vectors + (size_t)i * INTEG_DIMS,
                                   INTEG_DIMS);
    indices[i] = i;
  }

  /* Partial selection sort for top-k */
  for (int i = 0; i < k && i < n_vectors; i++) {
    int min_idx = i;
    for (int j = i + 1; j < n_vectors; j++) {
      if (dists[indices[j]] < dists[indices[min_idx]]) {
        min_idx = j;
      }
    }
    int tmp = indices[i];
    indices[i] = indices[min_idx];
    indices[min_idx] = tmp;
  }

  int n_results = k < n_vectors ? k : n_vectors;
  for (int i = 0; i < n_results; i++) {
    out_ids[i] = indices[i] + 1; /* IDs are 1-based */
    out_distances[i] = dists[indices[i]];
  }

  free(dists);
  free(indices);
}

/**************************************************************************
** 1. Reopen persistence
**
** Verify that data inserted into an index survives close + reopen.
** This exercises the full lifecycle: create → open → insert → search →
** close → reopen → search again → compare results.
**************************************************************************/

void test_integration_reopen_persistence(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = integ_config();
  DiskAnnIndex *idx = create_and_open(db, "test_reopen", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  int n_vectors = 100;
  float *vectors = gen_vectors(n_vectors, 42);

  /* Insert 100 vectors */
  for (int i = 0; i < n_vectors; i++) {
    int rc = diskann_insert(idx, (int64_t)(i + 1),
                            vectors + (size_t)i * INTEG_DIMS, INTEG_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  /* Search before close */
  float *query = vectors; /* query = first vector (should find id=1) */
  DiskAnnResult results_before[5];
  int n_before = diskann_search(idx, query, INTEG_DIMS, 5, results_before);
  TEST_ASSERT_TRUE(n_before >= 1);
  TEST_ASSERT_EQUAL_INT64(1, results_before[0].id);

  /* Close index (but keep db open) */
  diskann_close_index(idx);
  idx = NULL;

  /* Reopen index */
  int rc = diskann_open_index(db, "main", "test_reopen", &idx);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx);

  /* Search again — should get same results */
  DiskAnnResult results_after[5];
  int n_after = diskann_search(idx, query, INTEG_DIMS, 5, results_after);
  TEST_ASSERT_TRUE(n_after >= 1);
  TEST_ASSERT_EQUAL_INT64(1, results_after[0].id);

  /* Verify result counts match */
  TEST_ASSERT_EQUAL_INT(n_before, n_after);

  /* Verify all result IDs match (order may differ due to random start) */
  for (int i = 0; i < n_before; i++) {
    int found = 0;
    for (int j = 0; j < n_after; j++) {
      if (results_before[i].id == results_after[j].id) {
        found = 1;
        break;
      }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "result ID missing after reopen");
  }

  diskann_close_index(idx);
  free(vectors);
  sqlite3_close(db);
}

/**************************************************************************
** 2. Clear then reinsert
**
** Verify that diskann_clear_index() wipes all vectors, then reinserting
** and searching works correctly. Exercises the clear → reinsert path
** that no existing test covers.
**************************************************************************/

void test_integration_clear_reinsert(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = integ_config();
  DiskAnnIndex *idx = create_and_open(db, "test_clear_reinsert", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  int n_vectors = 20;
  float *vectors = gen_vectors(n_vectors, 99);

  /* Insert 20 vectors */
  for (int i = 0; i < n_vectors; i++) {
    int rc = diskann_insert(idx, (int64_t)(i + 1),
                            vectors + (size_t)i * INTEG_DIMS, INTEG_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "initial insert failed");
  }

  /* Search — should find results */
  float *query = vectors; /* first vector */
  DiskAnnResult results[5];
  int n = diskann_search(idx, query, INTEG_DIMS, 5, results);
  TEST_ASSERT_TRUE_MESSAGE(n >= 1, "search should find results before clear");

  /* Close index before clearing (blob handles would become stale) */
  diskann_close_index(idx);
  idx = NULL;

  /* Clear index — wipes all vector data but preserves metadata */
  int rc = diskann_clear_index(db, "main", "test_clear_reinsert");
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  /* Reopen index */
  rc = diskann_open_index(db, "main", "test_clear_reinsert", &idx);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx);

  /* Search on cleared index — should return 0 results */
  n = diskann_search(idx, query, INTEG_DIMS, 5, results);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, n,
                                "search on cleared index should return 0");

  /* Reinsert the same 20 vectors */
  for (int i = 0; i < n_vectors; i++) {
    rc = diskann_insert(idx, (int64_t)(i + 1), vectors + (size_t)i * INTEG_DIMS,
                        INTEG_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "reinsert failed");
  }

  /* Search again — should find results */
  n = diskann_search(idx, query, INTEG_DIMS, 5, results);
  TEST_ASSERT_TRUE_MESSAGE(n >= 1, "search should find results after reinsert");
  TEST_ASSERT_EQUAL_INT64(1, results[0].id);

  diskann_close_index(idx);
  free(vectors);
  sqlite3_close(db);
}

/**************************************************************************
** 3. Higher-dimension recall
**
** Insert 200 seeded random 128D vectors, query 20 random points, compare
** results against brute-force. Recall@10 >= 80%.
**
** This is the key quality metric. With random entry point (no medoid),
** 80% is a realistic lower bound on a 200-vector graph.
**************************************************************************/

void test_integration_recall_128d(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = integ_config();
  DiskAnnIndex *idx = create_and_open(db, "test_recall128", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  int n_vectors = 200;
  int n_queries = 20;
  int k = 10;
  float *vectors = gen_vectors(n_vectors, 12345);

  /* Insert all vectors */
  for (int i = 0; i < n_vectors; i++) {
    int rc = diskann_insert(idx, (int64_t)(i + 1),
                            vectors + (size_t)i * INTEG_DIMS, INTEG_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  /* Generate query vectors (different seed so they're not in the index) */
  float *queries = gen_vectors(n_queries, 67890);

  int total_hits = 0;
  int total_possible = 0;

  for (int q = 0; q < n_queries; q++) {
    float *query = queries + (size_t)q * INTEG_DIMS;

    /* Brute-force reference */
    int64_t bf_ids[10];
    float bf_dists[10];
    brute_force_knn(vectors, n_vectors, query, k, bf_ids, bf_dists);

    /* ANN search */
    DiskAnnResult ann_results[10];
    int n = diskann_search(idx, query, INTEG_DIMS, k, ann_results);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "search returned 0 results");

    /* Count hits: how many of the true top-k are in the ANN results? */
    int actual_k = k < n ? k : n;
    for (int i = 0; i < actual_k; i++) {
      for (int j = 0; j < n; j++) {
        if (bf_ids[i] == ann_results[j].id) {
          total_hits++;
          break;
        }
      }
    }
    total_possible += actual_k;
  }

  float recall = (float)total_hits / (float)total_possible;

  /* With 200 vectors, 128D, max_neighbors=16, recall should be >= 80% */
  char msg[128];
  snprintf(msg, sizeof(msg), "recall@%d = %.1f%% (expected >= 80%%)", k,
           (double)recall * 100.0);
  TEST_ASSERT_TRUE_MESSAGE(recall >= 0.8f, msg);

  diskann_close_index(idx);
  free(vectors);
  free(queries);
  sqlite3_close(db);
}

/**************************************************************************
** 4. Delete at scale
**
** Insert 50 vectors, delete 10, then search and verify:
** - No deleted IDs appear in results
** - Search still works and returns reasonable results
** - Recall on the remaining 40 vectors is acceptable
**************************************************************************/

void test_integration_delete_at_scale(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = integ_config();
  DiskAnnIndex *idx = create_and_open(db, "test_del_scale", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  int n_vectors = 50;
  int n_delete = 10;
  int k = 10;
  float *vectors = gen_vectors(n_vectors, 54321);

  /* Insert all 50 vectors */
  for (int i = 0; i < n_vectors; i++) {
    int rc = diskann_insert(idx, (int64_t)(i + 1),
                            vectors + (size_t)i * INTEG_DIMS, INTEG_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  /* Delete IDs 1 through 10 */
  for (int i = 1; i <= n_delete; i++) {
    int rc = diskann_delete(idx, (int64_t)i);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "delete failed");
  }

  /* Build brute-force reference over remaining vectors (IDs 11-50) */
  int n_remaining = n_vectors - n_delete;
  float *remaining_vecs =
      (float *)malloc((size_t)n_remaining * INTEG_DIMS * sizeof(float));
  TEST_ASSERT_NOT_NULL(remaining_vecs);
  memcpy(remaining_vecs, vectors + (size_t)n_delete * INTEG_DIMS,
         (size_t)n_remaining * INTEG_DIMS * sizeof(float));

  /* Query with 5 different vectors from the remaining set */
  int n_queries = 5;
  int total_hits = 0;
  int total_possible = 0;

  for (int q = 0; q < n_queries; q++) {
    /* Use remaining vectors as queries (indices 0..4 in remaining_vecs) */
    float *query = remaining_vecs + (size_t)q * INTEG_DIMS;

    /* ANN search */
    DiskAnnResult ann_results[10];
    int n = diskann_search(idx, query, INTEG_DIMS, k, ann_results);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "search returned 0 results after delete");

    /* Verify no deleted IDs in results */
    for (int i = 0; i < n; i++) {
      TEST_ASSERT_TRUE_MESSAGE(ann_results[i].id > n_delete,
                               "deleted ID appeared in search results");
    }

    /* Brute-force on remaining vectors */
    int64_t bf_ids[10];
    float bf_dists[10];
    brute_force_knn(remaining_vecs, n_remaining, query, k, bf_ids, bf_dists);

    /* Adjust bf_ids: brute_force_knn returns 1-based indices into
     * remaining_vecs, but real IDs are n_delete+1 based */
    for (int i = 0; i < k && i < n_remaining; i++) {
      bf_ids[i] += n_delete;
    }

    /* Count hits */
    int actual_k = k < n ? k : n;
    if (actual_k > n_remaining)
      actual_k = n_remaining;
    for (int i = 0; i < actual_k; i++) {
      for (int j = 0; j < n; j++) {
        if (bf_ids[i] == ann_results[j].id) {
          total_hits++;
          break;
        }
      }
    }
    total_possible += actual_k;
  }

  float recall = (float)total_hits / (float)total_possible;

  /* After deleting 20% of nodes, recall may drop but should still be OK */
  char msg[128];
  snprintf(msg, sizeof(msg),
           "post-delete recall@%d = %.1f%% (expected >= 60%%)", k,
           (double)recall * 100.0);
  TEST_ASSERT_TRUE_MESSAGE(recall >= 0.6f, msg);

  diskann_close_index(idx);
  free(vectors);
  free(remaining_vecs);
  sqlite3_close(db);
}
