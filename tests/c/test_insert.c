/*
** Test suite for diskann_insert()
**
** Tests the vector insertion algorithm including graph construction,
** edge pruning, and integration with search.
*/
#include "unity/unity.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/diskann.h"
#include "../../src/diskann_blob.h"
#include "../../src/diskann_internal.h"
#include "../../src/diskann_node.h"

#define TEST_DIMS 3
#define TEST_INDEX_NAME "test_insert"

/**************************************************************************
** Helpers
**************************************************************************/

static sqlite3 *open_db(void) {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  return db;
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

/* Count rows in shadow table */
static int count_shadow_rows(sqlite3 *db, const char *index_name) {
  char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM %s_shadow", index_name);
  TEST_ASSERT_NOT_NULL(sql);

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  rc = sqlite3_step(stmt);
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, rc);
  int count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

/* Read the number of edges for a given row from the shadow table */
static int get_edge_count(DiskAnnIndex *idx, int64_t rowid) {
  BlobSpot *spot = NULL;
  int rc = blob_spot_create(idx, &spot, (uint64_t)rowid, idx->block_size,
                            DISKANN_BLOB_READONLY);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  rc = blob_spot_reload(idx, spot, (uint64_t)rowid, idx->block_size);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  int n_edges = (int)node_bin_edges(idx, spot);
  blob_spot_free(spot);
  return n_edges;
}

/* Check if node A has an edge pointing to node B */
static int has_edge_to(DiskAnnIndex *idx, int64_t from, int64_t to) {
  BlobSpot *spot = NULL;
  int rc = blob_spot_create(idx, &spot, (uint64_t)from, idx->block_size,
                            DISKANN_BLOB_READONLY);
  if (rc != DISKANN_OK)
    return 0;
  rc = blob_spot_reload(idx, spot, (uint64_t)from, idx->block_size);
  if (rc != DISKANN_OK) {
    blob_spot_free(spot);
    return 0;
  }

  int found = node_bin_edge_find_idx(idx, spot, (uint64_t)to);
  blob_spot_free(spot);
  return found >= 0;
}

/**************************************************************************
** Validation tests
**************************************************************************/

void test_insert_null_index(void) {
  float vec[] = {1.0f, 0.0f, 0.0f};
  int rc = diskann_insert(NULL, 1, vec, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_ERROR_INVALID, rc);
}

void test_insert_null_vector(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_null_vec", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  int rc = diskann_insert(idx, 1, NULL, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_ERROR_INVALID, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_insert_dimension_mismatch(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_dim_mismatch", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[] = {1.0f, 0.0f};
  int rc = diskann_insert(idx, 1, vec, 2); /* wrong dimensions */
  TEST_ASSERT_EQUAL_INT(DISKANN_ERROR_DIMENSION, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** First insertion (empty index)
**************************************************************************/

void test_insert_first_vector(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_first", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[] = {1.0f, 2.0f, 3.0f};
  int rc = diskann_insert(idx, 1, vec, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  /* Verify: 1 row in shadow table */
  TEST_ASSERT_EQUAL_INT(1, count_shadow_rows(db, "test_first"));

  /* Verify: node has 0 edges (first node has nobody to connect to) */
  TEST_ASSERT_EQUAL_INT(0, get_edge_count(idx, 1));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Two-vector insertion (bidirectional edges)
**************************************************************************/

void test_insert_two_vectors(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_two", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  float v1[] = {1.0f, 0.0f, 0.0f};
  float v2[] = {0.0f, 1.0f, 0.0f};

  int rc = diskann_insert(idx, 1, v1, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  rc = diskann_insert(idx, 2, v2, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  /* Verify: 2 rows in shadow table */
  TEST_ASSERT_EQUAL_INT(2, count_shadow_rows(db, "test_two"));

  /* Verify: bidirectional edges */
  TEST_ASSERT_TRUE(has_edge_to(idx, 1, 2));
  TEST_ASSERT_TRUE(has_edge_to(idx, 2, 1));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Duplicate ID
**************************************************************************/

void test_insert_duplicate_id(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_dup", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  float vec[] = {1.0f, 0.0f, 0.0f};
  int rc = diskann_insert(idx, 1, vec, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  /* Second insert with same ID should fail */
  float vec2[] = {0.0f, 1.0f, 0.0f};
  rc = diskann_insert(idx, 1, vec2, TEST_DIMS);
  TEST_ASSERT_EQUAL_INT(DISKANN_ERROR_EXISTS, rc);

  /* Should still have exactly 1 row */
  TEST_ASSERT_EQUAL_INT(1, count_shadow_rows(db, "test_dup"));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Multi-vector insertion + search integration
**************************************************************************/

void test_insert_ten_vectors_searchable(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_ten", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert 10 vectors along the x-axis: (i, 0, 0) for i=1..10 */
  for (int i = 1; i <= 10; i++) {
    float vec[] = {(float)i, 0.0f, 0.0f};
    int rc = diskann_insert(idx, (int64_t)i, vec, TEST_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  TEST_ASSERT_EQUAL_INT(10, count_shadow_rows(db, "test_ten"));

  /* Search for (5, 0, 0) — should find id=5 as nearest */
  float query[] = {5.0f, 0.0f, 0.0f};
  DiskAnnResult results[3];
  int n = diskann_search(idx, query, TEST_DIMS, 3, results);
  TEST_ASSERT_TRUE(n >= 1);
  TEST_ASSERT_EQUAL_INT64(5, results[0].id);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, results[0].distance);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Edge count respects max_neighbors
**************************************************************************/

void test_insert_edge_count_limit(void) {
  sqlite3 *db = open_db();
  /* Use max_neighbors=4 so we can easily exceed it */
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 4,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_limit", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert 10 vectors */
  for (int i = 1; i <= 10; i++) {
    float vec[] = {(float)i, 0.0f, 0.0f};
    int rc = diskann_insert(idx, (int64_t)i, vec, TEST_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  /* Check that no node exceeds max_neighbors edges */
  uint32_t max_edges = node_edges_max_count(idx);
  for (int i = 1; i <= 10; i++) {
    int edges = get_edge_count(idx, (int64_t)i);
    TEST_ASSERT_TRUE_MESSAGE(edges <= (int)max_edges, "edge count exceeds max");
  }

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Recall test: insert random vectors and verify search quality
**************************************************************************/

void test_insert_recall(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 30,
                       .insert_list_size = 40,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_recall", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert 50 vectors with known positions */
  int n_vectors = 50;
  float vectors[50][3];
  srand(42);
  for (int i = 0; i < n_vectors; i++) {
    vectors[i][0] = (float)rand() / (float)RAND_MAX;
    vectors[i][1] = (float)rand() / (float)RAND_MAX;
    vectors[i][2] = (float)rand() / (float)RAND_MAX;
    int rc = diskann_insert(idx, (int64_t)(i + 1), vectors[i], TEST_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "insert failed");
  }

  /* Search for a known vector — brute-force compute top-5 */
  float query[] = {0.5f, 0.5f, 0.5f};
  int k = 5;

  /* Brute-force reference */
  typedef struct {
    int64_t id;
    float dist;
  } BFResult;
  BFResult bf[50];
  for (int i = 0; i < n_vectors; i++) {
    bf[i].id = (int64_t)(i + 1);
    bf[i].dist = diskann_distance_l2(query, vectors[i], TEST_DIMS);
  }
  /* Simple selection of top-k */
  for (int i = 0; i < k; i++) {
    for (int j = i + 1; j < n_vectors; j++) {
      if (bf[j].dist < bf[i].dist) {
        BFResult tmp = bf[i];
        bf[i] = bf[j];
        bf[j] = tmp;
      }
    }
  }

  /* ANN search */
  DiskAnnResult results[5];
  int n = diskann_search(idx, query, TEST_DIMS, k, results);
  TEST_ASSERT_TRUE(n >= k);

  /* Compute recall: how many of the true top-k are in the ANN results? */
  int hits = 0;
  for (int i = 0; i < k; i++) {
    for (int j = 0; j < n; j++) {
      if (bf[i].id == results[j].id) {
        hits++;
        break;
      }
    }
  }

  float recall = (float)hits / (float)k;
  /* With only 50 vectors and 3D, recall should be very high */
  TEST_ASSERT_TRUE_MESSAGE(recall >= 0.6f, "recall too low (expected >= 60%)");

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Insert then delete then search (integration)
**************************************************************************/

void test_insert_delete_search(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_ids", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert 5 vectors */
  float v1[] = {1.0f, 0.0f, 0.0f};
  float v2[] = {2.0f, 0.0f, 0.0f};
  float v3[] = {3.0f, 0.0f, 0.0f};
  float v4[] = {4.0f, 0.0f, 0.0f};
  float v5[] = {5.0f, 0.0f, 0.0f};

  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 1, v1, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 2, v2, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 3, v3, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 4, v4, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 5, v5, TEST_DIMS));

  /* Delete vector 3 */
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_delete(idx, 3));
  TEST_ASSERT_EQUAL_INT(4, count_shadow_rows(db, "test_ids"));

  /* Search for (3, 0, 0) — should NOT find id=3 */
  float query[] = {3.0f, 0.0f, 0.0f};
  DiskAnnResult results[4];
  int n = diskann_search(idx, query, TEST_DIMS, 4, results);
  TEST_ASSERT_TRUE(n >= 1);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_NOT_EQUAL(3, results[i].id);
  }

  diskann_close_index(idx);
  sqlite3_close(db);
}

/**************************************************************************
** Cosine metric insertion
**************************************************************************/

void test_insert_cosine_metric(void) {
  sqlite3 *db = open_db();
  DiskAnnConfig cfg = {.dimensions = TEST_DIMS,
                       .metric = DISKANN_METRIC_COSINE,
                       .max_neighbors = 8,
                       .search_list_size = 20,
                       .insert_list_size = 30,
                       .block_size = 0};
  DiskAnnIndex *idx = create_and_open(db, "test_cosine", &cfg);
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert vectors in different directions */
  float v1[] = {1.0f, 0.0f, 0.0f}; /* x-axis */
  float v2[] = {0.0f, 1.0f, 0.0f}; /* y-axis */
  float v3[] = {0.7f, 0.7f, 0.0f}; /* diagonal */

  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 1, v1, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 2, v2, TEST_DIMS));
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, diskann_insert(idx, 3, v3, TEST_DIMS));

  /* Search for diagonal direction — should find v3 first (same direction) */
  float query[] = {0.5f, 0.5f, 0.0f};
  DiskAnnResult results[3];
  int n = diskann_search(idx, query, TEST_DIMS, 3, results);
  TEST_ASSERT_TRUE(n >= 1);
  TEST_ASSERT_EQUAL_INT64(3, results[0].id);

  diskann_close_index(idx);
  sqlite3_close(db);
}
