/*
** Tests for diskann_delete() — vector deletion from DiskANN graph index.
**
** Copyright 2025 PhotoStructure Inc.
** MIT License
*/
#include "unity/unity.h"
#include "../../src/diskann.h"
#include "../../src/diskann_blob.h"
#include "../../src/diskann_internal.h"
#include "../../src/diskann_node.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/*
** Test configuration: 3D vectors, 256-byte blocks.
** This gives 8 max edges per node — enough for testing.
*/
#define TEST_DIMS 3
#define TEST_BLOCK_SIZE 256

/* ========================================================================
** Helpers
** ======================================================================== */

/*
** Create and open a test index with 3D / 256-byte-block config.
** Returns NULL on failure.
*/
static DiskAnnIndex *create_and_open_test_index(sqlite3 *db,
                                                const char *name) {
  DiskAnnConfig config = {.dimensions = TEST_DIMS,
                          .metric = DISKANN_METRIC_EUCLIDEAN,
                          .max_neighbors = 8,
                          .search_list_size = 100,
                          .insert_list_size = 200,
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

/*
** Insert a node BLOB into the shadow table with properly formatted data.
**
** Constructs a valid node BLOB using node_bin_init + node_bin_replace_edge,
** then INSERTs into the shadow table. Edges are optional (n_edges can be 0).
**
** Parameters:
**   db             - SQLite database
**   idx            - Open DiskAnnIndex (for layout params)
**   id             - Row ID for the new node
**   vector         - Node vector (TEST_DIMS floats)
**   edge_rowids    - Array of neighbor rowids (NULL if n_edges == 0)
**   edge_vectors   - Array of edge vectors, each TEST_DIMS floats (NULL if 0)
**   edge_distances - Array of edge distances (NULL if n_edges == 0)
**   n_edges        - Number of edges to add
**
** Returns SQLITE_OK on success.
*/
static int insert_node(sqlite3 *db, DiskAnnIndex *idx, int64_t id,
                       const float *vector, const uint64_t *edge_rowids,
                       const float *edge_vectors, const float *edge_distances,
                       int n_edges) {
  /* Allocate buffer for the node BLOB */
  uint8_t *buf = (uint8_t *)calloc(1, idx->block_size);
  if (!buf)
    return SQLITE_NOMEM;

  /* Create a stack-allocated BlobSpot over the buffer (no real BLOB handle) */
  BlobSpot spot;
  memset(&spot, 0, sizeof(spot));
  spot.buffer = buf;
  spot.buffer_size = idx->block_size;
  spot.is_writable = 1;
  spot.is_initialized = 1;
  spot.rowid = (uint64_t)id;

  /* Initialize node: write rowid + vector */
  node_bin_init(idx, &spot, (uint64_t)id, vector);

  /* Add edges */
  for (int i = 0; i < n_edges; i++) {
    node_bin_replace_edge(idx, &spot, i, edge_rowids[i], edge_distances[i],
                          edge_vectors + (size_t)i * TEST_DIMS);
  }

  /* INSERT into shadow table */
  char *sql =
      sqlite3_mprintf("INSERT INTO %s_shadow (id, data) VALUES (?, ?)",
                       idx->index_name);
  if (!sql) {
    free(buf);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    free(buf);
    return rc;
  }

  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_blob(stmt, 2, buf, (int)idx->block_size, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(buf);

  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

/*
** Count rows in the shadow table.
*/
static int count_shadow_rows(sqlite3 *db, const char *index_name) {
  char *sql =
      sqlite3_mprintf("SELECT COUNT(*) FROM %s_shadow", index_name);
  if (!sql)
    return -1;

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK)
    return -1;

  rc = sqlite3_step(stmt);
  int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return count;
}

/*
** Read a node's edge count from the shadow table (via BlobSpot).
*/
static int read_edge_count(DiskAnnIndex *idx, int64_t id) {
  BlobSpot *spot = NULL;
  int rc = blob_spot_create(idx, &spot, (uint64_t)id, idx->block_size, 0);
  if (rc != DISKANN_OK)
    return -1;

  rc = blob_spot_reload(idx, spot, (uint64_t)id, idx->block_size);
  if (rc != DISKANN_OK) {
    blob_spot_free(spot);
    return -1;
  }

  int count = (int)node_bin_edges(idx, spot);
  blob_spot_free(spot);
  return count;
}

/*
** Check if a node has an edge to a specific target rowid.
** Returns 1 if found, 0 if not, -1 on error.
*/
static int has_edge_to(DiskAnnIndex *idx, int64_t node_id,
                       int64_t target_id) {
  BlobSpot *spot = NULL;
  int rc =
      blob_spot_create(idx, &spot, (uint64_t)node_id, idx->block_size, 0);
  if (rc != DISKANN_OK)
    return -1;

  rc = blob_spot_reload(idx, spot, (uint64_t)node_id, idx->block_size);
  if (rc != DISKANN_OK) {
    blob_spot_free(spot);
    return -1;
  }

  int found =
      node_bin_edge_find_idx(idx, spot, (uint64_t)target_id) != -1 ? 1 : 0;
  blob_spot_free(spot);
  return found;
}

/* ========================================================================
** Tests
** ======================================================================== */

/*
** Delete with NULL index → DISKANN_ERROR_INVALID
*/
void test_delete_null_index(void) {
  int rc = diskann_delete(NULL, 1);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
}

/*
** Delete from empty index → DISKANN_ERROR_NOTFOUND
*/
void test_delete_from_empty_index(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  rc = diskann_delete(idx, 1);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Delete non-existent ID (index has a different row) → DISKANN_ERROR_NOTFOUND
*/
void test_delete_nonexistent_id(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert node with id=1 */
  float vec[] = {1.0f, 0.0f, 0.0f};
  rc = insert_node(db, idx, 1, vec, NULL, NULL, NULL, 0);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Try to delete id=999 */
  rc = diskann_delete(idx, 999);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

  /* Original row still exists */
  TEST_ASSERT_EQUAL(1, count_shadow_rows(db, "test_idx"));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Delete a single node with no edges → row removed, table still exists
*/
void test_delete_single_node_no_edges(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert node with id=42, no edges */
  float vec[] = {1.0f, 2.0f, 3.0f};
  rc = insert_node(db, idx, 42, vec, NULL, NULL, NULL, 0);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);
  TEST_ASSERT_EQUAL(1, count_shadow_rows(db, "test_idx"));

  /* Delete it */
  rc = diskann_delete(idx, 42);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Shadow table empty */
  TEST_ASSERT_EQUAL(0, count_shadow_rows(db, "test_idx"));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Delete a node with edges — back-edges cleaned from neighbors.
**
** Graph: A(1) ↔ B(2), A(1) ↔ C(3)   (bidirectional edges)
** Delete A → B should lose edge to A, C should lose edge to A.
*/
void test_delete_node_cleans_backedges(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  float vec_a[] = {1.0f, 0.0f, 0.0f};
  float vec_b[] = {0.0f, 1.0f, 0.0f};
  float vec_c[] = {0.0f, 0.0f, 1.0f};

  /* Node A (id=1): edges to B(2) and C(3) */
  uint64_t a_edge_ids[] = {2, 3};
  float a_edge_vecs[] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  float a_edge_dists[] = {1.0f, 1.0f};
  rc = insert_node(db, idx, 1, vec_a, a_edge_ids, a_edge_vecs, a_edge_dists,
                   2);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Node B (id=2): edge to A(1) */
  uint64_t b_edge_ids[] = {1};
  float b_edge_vecs[] = {1.0f, 0.0f, 0.0f};
  float b_edge_dists[] = {1.0f};
  rc = insert_node(db, idx, 2, vec_b, b_edge_ids, b_edge_vecs, b_edge_dists,
                   1);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Node C (id=3): edge to A(1) */
  uint64_t c_edge_ids[] = {1};
  float c_edge_vecs[] = {1.0f, 0.0f, 0.0f};
  float c_edge_dists[] = {1.0f};
  rc = insert_node(db, idx, 3, vec_c, c_edge_ids, c_edge_vecs, c_edge_dists,
                   1);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Verify initial state */
  TEST_ASSERT_EQUAL(3, count_shadow_rows(db, "test_idx"));
  TEST_ASSERT_EQUAL(1, has_edge_to(idx, 2, 1)); /* B → A */
  TEST_ASSERT_EQUAL(1, has_edge_to(idx, 3, 1)); /* C → A */

  /* Delete A */
  rc = diskann_delete(idx, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* A is gone */
  TEST_ASSERT_EQUAL(2, count_shadow_rows(db, "test_idx"));

  /* B and C no longer have edges to A */
  TEST_ASSERT_EQUAL(0, has_edge_to(idx, 2, 1)); /* B → A removed */
  TEST_ASSERT_EQUAL(0, has_edge_to(idx, 3, 1)); /* C → A removed */

  /* B and C edge counts decremented */
  TEST_ASSERT_EQUAL(0, read_edge_count(idx, 2));
  TEST_ASSERT_EQUAL(0, read_edge_count(idx, 3));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Delete the last node → index empty but functional
*/
void test_delete_last_node(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  float vec[] = {1.0f, 2.0f, 3.0f};
  rc = insert_node(db, idx, 1, vec, NULL, NULL, NULL, 0);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  rc = diskann_delete(idx, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Index is empty */
  TEST_ASSERT_EQUAL(0, count_shadow_rows(db, "test_idx"));

  /* Deleting again should fail */
  rc = diskann_delete(idx, 1);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Double delete → second returns DISKANN_ERROR_NOTFOUND
*/
void test_delete_double_delete(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  float vec[] = {1.0f, 0.0f, 0.0f};
  rc = insert_node(db, idx, 5, vec, NULL, NULL, NULL, 0);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* First delete succeeds */
  rc = diskann_delete(idx, 5);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Second delete fails */
  rc = diskann_delete(idx, 5);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Zombie edge: A has an edge to B, but B was already deleted from the table.
** Delete A should succeed — skip the zombie edge gracefully.
*/
void test_delete_zombie_edge(void) {
  sqlite3 *db = NULL;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnIndex *idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  float vec_a[] = {1.0f, 0.0f, 0.0f};
  float vec_b[] = {0.0f, 1.0f, 0.0f};

  /* Insert A with edge to B (id=2), but do NOT insert B */
  uint64_t a_edge_ids[] = {2};
  float a_edge_vecs[] = {0.0f, 1.0f, 0.0f};
  float a_edge_dists[] = {1.0f};
  rc = insert_node(db, idx, 1, vec_a, a_edge_ids, a_edge_vecs, a_edge_dists,
                   1);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);
  (void)vec_b; /* B not inserted — simulates already-deleted node */

  /* Delete A should succeed despite zombie edge to non-existent B */
  rc = diskann_delete(idx, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  TEST_ASSERT_EQUAL(0, count_shadow_rows(db, "test_idx"));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/* main() is in test_runner.c */
