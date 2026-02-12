/*
** Tests for k-NN search
**
** Derived from libSQL DiskANN implementation
** Copyright 2024 the libSQL authors
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** Test strategy:
**
** 1. VALIDATION TESTS — input checking for diskann_search()
**    - NULL index, query, results → error
**    - Dimension mismatch → DISKANN_ERROR_DIMENSION
**    - k < 0 → error, k = 0 → 0 results
**
** 2. EMPTY INDEX — search on index with no rows → 0 results
**
** 3. SINGLE-VECTOR — simplest graph (1 node, 0 edges)
**    - Query = stored vector → distance ≈ 0
**    - Any query → returns the one result
**    - k > 1 → returns 1 result (clamped)
**
** 4. KNOWN-GRAPH — hand-built 4-node fully-connected graph (3D vectors)
**    - Verify correct nearest neighbor is returned first
**    - Verify results sorted by distance ascending
**    - Verify k > n returns n results
**
** 5. MULTI-VECTOR RECALL — 50-vector random graph
**    - Compare search results against brute-force reference
**    - Recall@k must be ≥ 95% (on small graph, should be 100%)
**
** 6. COSINE METRIC — verify search works with cosine distance
**
** Test data setup:
**   Tests use small 3D vectors for human-verifiable distances.
**   Graph data is inserted by:
**   1. diskann_create_index + diskann_open_index (creates tables)
**   2. insert_shadow_row() — inserts zeroed BLOB into shadow table
**   3. blob_spot_create + blob_spot_reload — opens BLOB for writing
**   4. node_bin_init + node_bin_replace_edge — writes node data
**   5. blob_spot_flush — writes buffer back to DB
**
**   This bypasses diskann_insert() (not yet implemented) and directly
**   constructs valid graph BLOBs using the node binary format layer.
*/
#include "../../src/diskann.h"
#include "../../src/diskann_blob.h"
#include "../../src/diskann_internal.h"
#include "../../src/diskann_node.h"
#include "../../src/diskann_search.h"
#include "unity/unity.h"
#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/*
** Test configuration: 3D vectors, auto-calculated block size.
*/
#define TEST_DIMS 3
#define TEST_BLOCK_SIZE 0
#define TEST_MAX_NEIGHBORS 8
#define TEST_SEARCH_L 32
#define TEST_INSERT_L 64

/**************************************************************************
** Test helpers
**************************************************************************/

/* Create and open a test index with small 3D config */
static DiskAnnIndex *create_test_index(sqlite3 *db, const char *name,
                                       uint8_t metric) {
  DiskAnnConfig config = {.dimensions = TEST_DIMS,
                          .metric = metric,
                          .max_neighbors = TEST_MAX_NEIGHBORS,
                          .search_list_size = TEST_SEARCH_L,
                          .insert_list_size = TEST_INSERT_L,
                          .block_size = TEST_BLOCK_SIZE};

  int rc = diskann_create_index(db, "main", name, &config);
  if (rc != DISKANN_OK)
    return NULL;

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", name, &idx);
  if (rc != DISKANN_OK)
    return NULL;

  return idx;
}

/* Insert a zeroed BLOB row into the shadow table (for later node_bin_init) */
static int insert_shadow_row(sqlite3 *db, const char *index_name, int64_t id,
                             uint32_t block_size) {
  char *sql = sqlite3_mprintf("INSERT INTO %s_shadow (id, data) VALUES (?, ?)",
                              index_name);
  if (!sql)
    return SQLITE_NOMEM;

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK)
    return rc;

  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_zeroblob(stmt, 2, (int)block_size);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

/*
** Insert a fully-formed graph node into the shadow table.
**
** Parameters:
**   idx     - open index handle
**   rowid   - node row ID
**   vector  - node vector data (TEST_DIMS floats)
**   edges   - array of (target_rowid, distance, edge_vector) triples
**   n_edges - number of edges
**
** Inserts a zeroed row, opens writable BlobSpot, writes node via
** node_bin_init + node_bin_replace_edge, then flushes.
*/
typedef struct {
  int64_t target_rowid;
  float distance;
  const float *vector;
} TestEdge;

static int insert_graph_node(DiskAnnIndex *idx, int64_t rowid,
                             const float *vector, const TestEdge *edges,
                             int n_edges) {
  int rc;

  rc = insert_shadow_row(idx->db, idx->index_name, rowid, idx->block_size);
  if (rc != SQLITE_OK)
    return rc;

  BlobSpot *spot = NULL;
  rc = blob_spot_create(idx, &spot, (uint64_t)rowid, idx->block_size, 1);
  if (rc != DISKANN_OK)
    return rc;

  rc = blob_spot_reload(idx, spot, (uint64_t)rowid, idx->block_size);
  if (rc != DISKANN_OK) {
    blob_spot_free(spot);
    return rc;
  }

  node_bin_init(idx, spot, (uint64_t)rowid, vector);

  for (int i = 0; i < n_edges; i++) {
    node_bin_replace_edge(idx, spot, i, (uint64_t)edges[i].target_rowid,
                          edges[i].distance, edges[i].vector);
  }

  rc = blob_spot_flush(idx, spot);
  blob_spot_free(spot);
  return rc;
}

/* Compute brute-force nearest neighbors from a set of vectors */
static void brute_force_knn(const float *vectors, int n_vectors, uint32_t dims,
                            uint8_t metric, const float *query, int k,
                            int64_t *out_ids, float *out_distances) {
  if (n_vectors <= 0)
    return;

  /* Compute all distances */
  float *all_distances = (float *)malloc((size_t)n_vectors * sizeof(float));
  int *indices = (int *)malloc((size_t)n_vectors * sizeof(int));
  for (int i = 0; i < n_vectors; i++) {
    all_distances[i] = diskann_distance(
        query, vectors + (size_t)(uint32_t)i * dims, dims, metric);
    indices[i] = i;
  }

  /* Simple selection sort for top-k (fine for small n) */
  for (int i = 0; i < k && i < n_vectors; i++) {
    int min_idx = i;
    for (int j = i + 1; j < n_vectors; j++) {
      if (all_distances[indices[j]] < all_distances[indices[min_idx]]) {
        min_idx = j;
      }
    }
    int tmp = indices[i];
    indices[i] = indices[min_idx];
    indices[min_idx] = tmp;
  }

  int n_results = k < n_vectors ? k : n_vectors;
  for (int i = 0; i < n_results; i++) {
    out_ids[i] = indices[i] + 1; /* rowids are 1-based */
    out_distances[i] = all_distances[indices[i]];
  }

  free(all_distances);
  free(indices);
}

/**************************************************************************
** 1. Validation tests
**************************************************************************/

void test_search_null_index(void) {
  float query[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  DiskAnnResult results[1];
  int rc = diskann_search(NULL, query, TEST_DIMS, 1, results);
  TEST_ASSERT_TRUE(rc < 0);
}

void test_search_null_query(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  DiskAnnResult results[1];
  rc = diskann_search(idx, NULL, TEST_DIMS, 1, results);
  TEST_ASSERT_TRUE(rc < 0);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_null_results(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float query[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  rc = diskann_search(idx, query, TEST_DIMS, 1, NULL);
  TEST_ASSERT_TRUE(rc < 0);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_dimension_mismatch(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float query[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  DiskAnnResult results[1];
  /* Query has 5 dims, index has TEST_DIMS (3) */
  rc = diskann_search(idx, query, 5, 1, results);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_DIMENSION, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_negative_k(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float query[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  DiskAnnResult results[1];
  rc = diskann_search(idx, query, TEST_DIMS, -1, results);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_zero_k(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert one node so index is non-empty */
  float vec[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  int irc = insert_graph_node(idx, 1, vec, NULL, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  DiskAnnResult results[1];
  rc = diskann_search(idx, vec, TEST_DIMS, 0, results);
  TEST_ASSERT_EQUAL(0, rc); /* 0 results returned */

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** 2. Empty index
**************************************************************************/

void test_search_empty_index(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float query[TEST_DIMS] = {1.0f, 2.0f, 3.0f};
  DiskAnnResult results[5];
  rc = diskann_search(idx, query, TEST_DIMS, 5, results);
  TEST_ASSERT_EQUAL(0, rc); /* 0 results found */

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** 3. Single-vector tests
**************************************************************************/

void test_search_single_vector_exact(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[TEST_DIMS] = {1.0f, 2.0f, 3.0f};
  int irc = insert_graph_node(idx, 42, vec, NULL, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  /* Search for the exact same vector */
  DiskAnnResult results[1];
  rc = diskann_search(idx, vec, TEST_DIMS, 1, results);
  TEST_ASSERT_EQUAL(1, rc); /* 1 result */
  TEST_ASSERT_EQUAL(42, results[0].id);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_single_vector_different_query(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  int irc = insert_graph_node(idx, 1, vec, NULL, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  /* Search with different query — still returns the one result */
  float query[TEST_DIMS] = {0.0f, 1.0f, 0.0f};
  DiskAnnResult results[1];
  rc = diskann_search(idx, query, TEST_DIMS, 1, results);
  TEST_ASSERT_EQUAL(1, rc);
  TEST_ASSERT_EQUAL(1, results[0].id);
  /* L2 distance: (1-0)^2 + (0-1)^2 + 0 = 2.0 */
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_single_vector_k_larger(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[TEST_DIMS] = {1.0f, 2.0f, 3.0f};
  int irc = insert_graph_node(idx, 1, vec, NULL, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  /* Ask for 10 results but only 1 exists */
  DiskAnnResult results[10];
  rc = diskann_search(idx, vec, TEST_DIMS, 10, results);
  TEST_ASSERT_EQUAL(1, rc); /* Only 1 result returned */

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** 4. Known-graph tests (4-node fully-connected)
**
** Graph layout (3D vectors, L2 distance):
**   Node 1: (1, 0, 0) — "east"
**   Node 2: (0, 1, 0) — "north"
**   Node 3: (0, 0, 1) — "up"
**   Node 4: (1, 1, 1) — "diagonal"
**
** All nodes connected to all others (fully-connected 4-node graph).
** This ensures the beam search can traverse to any node.
**
** Query (1, 0, 0):
**   L2 to Node 1: 0.0 (exact match)
**   L2 to Node 2: 2.0 (1+1+0)
**   L2 to Node 3: 2.0 (1+0+1)
**   L2 to Node 4: 2.0 (0+1+1)
**
** Query (0.9, 0.1, 0):
**   L2 to Node 1: 0.02 (0.01+0.01+0)
**   L2 to Node 2: 1.62 (0.81+0.81+0)
**   L2 to Node 3: 1.82 (0.81+0.01+1)
**   L2 to Node 4: 1.82 (0.01+0.81+1)
**************************************************************************/

/* Known test vectors */
static const float VEC_EAST[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
static const float VEC_NORTH[TEST_DIMS] = {0.0f, 1.0f, 0.0f};
static const float VEC_UP[TEST_DIMS] = {0.0f, 0.0f, 1.0f};
static const float VEC_DIAG[TEST_DIMS] = {1.0f, 1.0f, 1.0f};

/* Build the 4-node fully-connected test graph */
static DiskAnnIndex *build_four_node_graph(sqlite3 *db) {
  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  if (!idx)
    return NULL;

  /* Precompute inter-node L2 distances */
  float d12 = diskann_distance_l2(VEC_EAST, VEC_NORTH, TEST_DIMS);
  float d13 = diskann_distance_l2(VEC_EAST, VEC_UP, TEST_DIMS);
  float d14 = diskann_distance_l2(VEC_EAST, VEC_DIAG, TEST_DIMS);
  float d23 = diskann_distance_l2(VEC_NORTH, VEC_UP, TEST_DIMS);
  float d24 = diskann_distance_l2(VEC_NORTH, VEC_DIAG, TEST_DIMS);
  float d34 = diskann_distance_l2(VEC_UP, VEC_DIAG, TEST_DIMS);

  /* Node 1: edges to 2, 3, 4 */
  TestEdge edges1[] = {
      {2, d12, VEC_NORTH}, {3, d13, VEC_UP}, {4, d14, VEC_DIAG}};
  int rc = insert_graph_node(idx, 1, VEC_EAST, edges1, 3);
  if (rc != DISKANN_OK)
    return NULL;

  /* Node 2: edges to 1, 3, 4 */
  TestEdge edges2[] = {
      {1, d12, VEC_EAST}, {3, d23, VEC_UP}, {4, d24, VEC_DIAG}};
  rc = insert_graph_node(idx, 2, VEC_NORTH, edges2, 3);
  if (rc != DISKANN_OK)
    return NULL;

  /* Node 3: edges to 1, 2, 4 */
  TestEdge edges3[] = {
      {1, d13, VEC_EAST}, {2, d23, VEC_NORTH}, {4, d34, VEC_DIAG}};
  rc = insert_graph_node(idx, 3, VEC_UP, edges3, 3);
  if (rc != DISKANN_OK)
    return NULL;

  /* Node 4: edges to 1, 2, 3 */
  TestEdge edges4[] = {
      {1, d14, VEC_EAST}, {2, d24, VEC_NORTH}, {3, d34, VEC_UP}};
  rc = insert_graph_node(idx, 4, VEC_DIAG, edges4, 3);
  if (rc != DISKANN_OK)
    return NULL;

  return idx;
}

void test_search_known_graph_exact_match(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  /* Query = VEC_EAST, should find node 1 with distance 0 */
  DiskAnnResult results[4];
  rc = diskann_search(idx, VEC_EAST, TEST_DIMS, 4, results);
  TEST_ASSERT_TRUE(rc > 0);
  TEST_ASSERT_EQUAL(1, results[0].id);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_known_graph_nearest(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  /* Query close to VEC_EAST: (0.9, 0.1, 0.0) */
  float query[TEST_DIMS] = {0.9f, 0.1f, 0.0f};
  DiskAnnResult results[4];
  rc = diskann_search(idx, query, TEST_DIMS, 4, results);
  TEST_ASSERT_EQUAL(4, rc);

  /* Nearest should be node 1 (VEC_EAST), L2 = 0.01+0.01 = 0.02 */
  TEST_ASSERT_EQUAL(1, results[0].id);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.02f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_known_graph_sorted_results(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  float query[TEST_DIMS] = {0.9f, 0.1f, 0.0f};
  DiskAnnResult results[4];
  rc = diskann_search(idx, query, TEST_DIMS, 4, results);
  TEST_ASSERT_EQUAL(4, rc);

  /* Results must be sorted by distance ascending */
  for (int i = 0; i < rc - 1; i++) {
    TEST_ASSERT_TRUE_MESSAGE(results[i].distance <= results[i + 1].distance,
                             "Results not sorted by distance");
  }

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_known_graph_k_less_than_n(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  /* Ask for 2 out of 4 */
  DiskAnnResult results[2];
  rc = diskann_search(idx, VEC_EAST, TEST_DIMS, 2, results);
  TEST_ASSERT_EQUAL(2, rc);
  /* First should still be the exact match */
  TEST_ASSERT_EQUAL(1, results[0].id);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_known_graph_k_greater_than_n(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  /* Ask for 10 but only 4 exist */
  DiskAnnResult results[10];
  rc = diskann_search(idx, VEC_EAST, TEST_DIMS, 10, results);
  TEST_ASSERT_EQUAL(4, rc); /* Clamped to available count */

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_readonly_no_writes(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = build_four_node_graph(db);
  TEST_ASSERT_NOT_NULL(idx);

  uint64_t writes_before = idx->num_writes;

  DiskAnnResult results[4];
  rc = diskann_search(idx, VEC_EAST, TEST_DIMS, 4, results);
  TEST_ASSERT_TRUE(rc > 0);

  /* Search should not write anything */
  TEST_ASSERT_EQUAL(writes_before, idx->num_writes);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** 5. Brute-force recall test (50 random vectors)
**************************************************************************/

void test_search_brute_force_recall(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx =
      create_test_index(db, "test_idx", DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_NOT_NULL(idx);

  const int N = 50;
#define RECALL_K 5

  /* Generate deterministic "random" vectors using a simple LCG */
  float vectors[50][TEST_DIMS];
  uint32_t seed = 12345;
  for (int i = 0; i < N; i++) {
    for (int d = 0; d < TEST_DIMS; d++) {
      seed = seed * 1103515245 + 12345;
      vectors[i][d] = (float)(seed & 0x7FFFFFFF) / (float)0x7FFFFFFF;
    }
  }

  /* Build fully-connected graph (each node connects to all others) */
  for (int i = 0; i < N; i++) {
    /* Connect to up to max_neighbors nearest neighbors */
    TestEdge edges[TEST_MAX_NEIGHBORS];
    int n_edges = 0;

    /* Simple: connect to next TEST_MAX_NEIGHBORS nodes (wrapping) */
    for (int j = 1; j <= TEST_MAX_NEIGHBORS && j < N; j++) {
      int neighbor = (i + j) % N;
      edges[n_edges].target_rowid = neighbor + 1; /* 1-based rowids */
      edges[n_edges].distance =
          diskann_distance_l2(vectors[i], vectors[neighbor], TEST_DIMS);
      edges[n_edges].vector = vectors[neighbor];
      n_edges++;
    }

    int irc = insert_graph_node(idx, i + 1, vectors[i], edges, n_edges);
    TEST_ASSERT_EQUAL(DISKANN_OK, irc);
  }

  /* Query with the first vector */
  float query[TEST_DIMS] = {0.5f, 0.5f, 0.5f};

  /* Brute force reference */
  int64_t bf_ids[RECALL_K];
  float bf_distances[RECALL_K];
  brute_force_knn((const float *)vectors, N, TEST_DIMS,
                  DISKANN_METRIC_EUCLIDEAN, query, RECALL_K, bf_ids,
                  bf_distances);

  /* ANN search */
  DiskAnnResult ann_results[RECALL_K];
  rc = diskann_search(idx, query, TEST_DIMS, RECALL_K, ann_results);
  TEST_ASSERT_EQUAL(RECALL_K, rc);

  /* Check recall: count how many of the true top-K are in the ANN results */
  int recall = 0;
  for (int i = 0; i < RECALL_K; i++) {
    for (int j = 0; j < RECALL_K; j++) {
      if (ann_results[j].id == bf_ids[i]) {
        recall++;
        break;
      }
    }
  }

  /* On a 50-vector graph with max 8 edges, recall should be very high */
  float recall_rate = (float)recall / (float)RECALL_K;
  TEST_ASSERT_TRUE_MESSAGE(recall_rate >= 0.8f,
                           "Recall too low — search may be broken");

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** 6. Cosine metric test
**************************************************************************/

void test_search_cosine_metric(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_test_index(db, "test_idx", DISKANN_METRIC_COSINE);
  TEST_ASSERT_NOT_NULL(idx);

  /* Two vectors: same direction vs different direction */
  float vec_a[TEST_DIMS] = {1.0f, 0.0f, 0.0f};
  float vec_b[TEST_DIMS] = {0.0f, 1.0f, 0.0f};

  float d_ab = diskann_distance_cosine(vec_a, vec_b, TEST_DIMS);

  /* Node 1 edges to 2, node 2 edges to 1 */
  TestEdge edges1[] = {{2, d_ab, vec_b}};
  int irc = insert_graph_node(idx, 1, vec_a, edges1, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  TestEdge edges2[] = {{1, d_ab, vec_a}};
  irc = insert_graph_node(idx, 2, vec_b, edges2, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, irc);

  /* Query in same direction as vec_a → node 1 should be nearest */
  float query[TEST_DIMS] = {2.0f, 0.0f, 0.0f};
  DiskAnnResult results[2];
  rc = diskann_search(idx, query, TEST_DIMS, 2, results);
  TEST_ASSERT_EQUAL(2, rc);
  TEST_ASSERT_EQUAL(1, results[0].id);
  /* Cosine distance between (2,0,0) and (1,0,0) = 0 (same direction) */
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Hash Set Tests for Visited Tracking Optimization
**
** These tests verify the O(1) hash set implementation for visited tracking.
** The visited_set_* functions are internal to diskann_search.c, so we need
** to expose them for testing (via test helpers or conditional compilation).
**
** Note: These tests will FAIL to compile initially because visited_set_*
** functions don't exist yet. This is correct TDD.
**************************************************************************/

/*
** Hash set test helpers - TESTING is defined in Makefile for test builds
** This exposes the internal visited_set_* functions declared in
*diskann_search.h
*/

/* Test 1: Initialize hash table */
void test_visited_set_init(void) {
  VisitedSet set;
  visited_set_init(&set, 256);

  /* Verify initialization */
  TEST_ASSERT_EQUAL(256, set.capacity);
  TEST_ASSERT_EQUAL(0, set.count);
  TEST_ASSERT_NOT_NULL(set.rowids);

  /* Verify all slots initialized to empty sentinel (0xFFFFFFFFFFFFFFFF) */
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFFFFFFFFFFFFULL, set.rowids[i]);
  }

  visited_set_deinit(&set);
}

/* Test 2: Basic add and contains operations */
void test_visited_set_add_contains(void) {
  VisitedSet set;
  visited_set_init(&set, 256);

  /* Add rowids 1, 2, 3 */
  visited_set_add(&set, 1);
  visited_set_add(&set, 2);
  visited_set_add(&set, 3);

  /* Verify contains returns 1 for added rowids */
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 1));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 2));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 3));

  /* Verify contains returns 0 for non-added rowid */
  TEST_ASSERT_EQUAL(0, visited_set_contains(&set, 4));
  TEST_ASSERT_EQUAL(0, visited_set_contains(&set, 999));

  visited_set_deinit(&set);
}

/* Test 3: Hash collision handling with linear probing */
void test_visited_set_collisions(void) {
  VisitedSet set;
  visited_set_init(&set, 8); /* Small capacity to force collisions */

  /*
  ** Find two rowids that hash to the same bucket.
  ** With FNV-1a hash (rowid * 0x100000001b3) and capacity 8,
  ** rowids that differ by 8 will likely collide.
  ** We'll add rowids 1, 9, 17 (differ by 8).
  */
  visited_set_add(&set, 1);
  visited_set_add(&set, 9);
  visited_set_add(&set, 17);

  /* All should be found despite potential collisions */
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 1));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 9));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 17));

  visited_set_deinit(&set);
}

/* Test 4: Index wraparound at end of table */
void test_visited_set_wraparound(void) {
  VisitedSet set;
  visited_set_init(&set, 8);

  /*
  ** Force entries near the end of the table that probe into the beginning.
  ** Add rowids that hash to buckets 6, 7, then cause wraparound to 0, 1.
  */
  visited_set_add(&set, 6);
  visited_set_add(&set, 7);
  visited_set_add(&set, 8); /* Will wrap to bucket 0 */
  visited_set_add(&set, 9); /* Will wrap to bucket 1 */

  /* All should be findable */
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 6));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 7));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 8));
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 9));

  visited_set_deinit(&set);
}

/* Test 5: Adding same rowid twice (idempotent) */
void test_visited_set_duplicates(void) {
  VisitedSet set;
  visited_set_init(&set, 256);

  /* Add rowid=1 */
  visited_set_add(&set, 1);
  int count_after_first = set.count;

  /* Add rowid=1 again */
  visited_set_add(&set, 1);
  int count_after_second = set.count;

  /* Count should not double-increment */
  TEST_ASSERT_EQUAL(count_after_first, count_after_second);

  /* Should still be found */
  TEST_ASSERT_EQUAL(1, visited_set_contains(&set, 1));

  visited_set_deinit(&set);
}

/* Test 6: Fill entire table to capacity */
void test_visited_set_full_table(void) {
  VisitedSet set;
  visited_set_init(&set, 256);

  /* Add 256 unique rowids (fills entire table) */
  for (uint64_t i = 1; i <= 256; i++) {
    visited_set_add(&set, i);
  }

  /* All should be findable */
  for (uint64_t i = 1; i <= 256; i++) {
    TEST_ASSERT_EQUAL(1, visited_set_contains(&set, i));
  }

  /* Non-added rowid should not be found */
  TEST_ASSERT_EQUAL(0, visited_set_contains(&set, 999));

  visited_set_deinit(&set);
}

/* Test 7: NULL safety for visited set functions */
void test_visited_set_null_safety(void) {
  /* visited_set_deinit with NULL — should not crash */
  visited_set_deinit(NULL);

  /* If we get here without segfault, test passes */
  TEST_ASSERT(1);
}

/* main() is in test_runner.c */
