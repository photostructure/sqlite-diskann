/*
** Tests for diskann_node.h/.c — node binary format, LE serialization,
** distance calculations, buffer management, and node alloc/free.
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "../../src/diskann_node.h"
#include "unity/unity.h"
#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
** LE serialization tests
** ======================================================================== */

void test_le16_roundtrip(void) {
  uint8_t buf[2];
  write_le16(buf, 0x1234);
  TEST_ASSERT_EQUAL_UINT16(0x1234, read_le16(buf));
  /* Verify byte order: least significant byte first */
  TEST_ASSERT_EQUAL_HEX8(0x34, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12, buf[1]);
}

void test_le32_roundtrip(void) {
  uint8_t buf[4];
  write_le32(buf, 0xDEADBEEF);
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, read_le32(buf));
  TEST_ASSERT_EQUAL_HEX8(0xEF, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0xBE, buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0xAD, buf[2]);
  TEST_ASSERT_EQUAL_HEX8(0xDE, buf[3]);
}

void test_le64_roundtrip(void) {
  uint8_t buf[8];
  write_le64(buf, 0x0102030405060708ULL);
  TEST_ASSERT_EQUAL_UINT64(0x0102030405060708ULL, read_le64(buf));
  TEST_ASSERT_EQUAL_HEX8(0x08, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x07, buf[1]);
  TEST_ASSERT_EQUAL_HEX8(0x06, buf[2]);
  TEST_ASSERT_EQUAL_HEX8(0x05, buf[3]);
  TEST_ASSERT_EQUAL_HEX8(0x04, buf[4]);
  TEST_ASSERT_EQUAL_HEX8(0x03, buf[5]);
  TEST_ASSERT_EQUAL_HEX8(0x02, buf[6]);
  TEST_ASSERT_EQUAL_HEX8(0x01, buf[7]);
}

void test_le16_zero(void) {
  uint8_t buf[2] = {0xFF, 0xFF};
  write_le16(buf, 0);
  TEST_ASSERT_EQUAL_UINT16(0, read_le16(buf));
  TEST_ASSERT_EQUAL_HEX8(0x00, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, buf[1]);
}

void test_le64_max(void) {
  uint8_t buf[8];
  write_le64(buf, UINT64_MAX);
  TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, read_le64(buf));
}

/* ========================================================================
** Layout calculation tests
** ======================================================================== */

/*
** Helper: create a DiskAnnIndex with test configuration.
** Does NOT need a real database — layout calculations are pure math.
*/
static DiskAnnIndex make_test_index(uint32_t dims, uint32_t block_size) {
  DiskAnnIndex idx;
  memset(&idx, 0, sizeof(idx));
  idx.dimensions = dims;
  idx.block_size = block_size;
  idx.nNodeVectorSize = dims * (uint32_t)sizeof(float);
  idx.nEdgeVectorSize = idx.nNodeVectorSize; /* float32-only */
  idx.metric = DISKANN_METRIC_EUCLIDEAN;
  idx.pruning_alpha = 1.2;
  return idx;
}

void test_max_edges_3d_256block(void) {
  /* 3D float32: nodeVectorSize = 12 bytes
   * nodeOverhead = 16 (metadata) + 12 (vector) = 28
   * edgeOverhead = 12 (vector) + 16 (metadata) = 28
   * maxEdges = (256 - 28) / 28 = 228 / 28 = 8 */
  DiskAnnIndex idx = make_test_index(3, 256);
  TEST_ASSERT_EQUAL_UINT32(8, node_edges_max_count(&idx));
}

void test_max_edges_768d_too_small(void) {
  /* 768D float32: nodeVectorSize = 3072 bytes
   * nodeOverhead = 16 + 3072 = 3088
   * 4096 - 3088 = 1008 available
   * edgeOverhead = 3072 + 16 = 3088
   * 1008 / 3088 = 0 edges — block too small! */
  DiskAnnIndex idx = make_test_index(768, 4096);
  TEST_ASSERT_EQUAL_UINT32(0, node_edges_max_count(&idx));
}

void test_max_edges_4d_large_block(void) {
  /* 4D float32: nodeVectorSize = 16 bytes
   * nodeOverhead = 16 + 16 = 32
   * edgeOverhead = 16 + 16 = 32
   * maxEdges = (4096 - 32) / 32 = 127 */
  DiskAnnIndex idx = make_test_index(4, 4096);
  TEST_ASSERT_EQUAL_UINT32(127, node_edges_max_count(&idx));
}

void test_metadata_offset_3d(void) {
  /* With 3D, 256 block, 8 max edges:
   * metadata_offset = 16 (nodeMetadata) + 12 (nodeVector) + 8 * 12
   * (edgeVectors) = 16 + 12 + 96 = 124 */
  DiskAnnIndex idx = make_test_index(3, 256);
  uint32_t offset = node_edges_metadata_offset(&idx);
  TEST_ASSERT_EQUAL_UINT32(124, offset);
  /* Check that metadata fits: 124 + 8 * 16 = 252 <= 256 */
  TEST_ASSERT_TRUE(offset + 8 * EDGE_METADATA_SIZE <= 256);
}

/* ========================================================================
** Node binary format tests
** ======================================================================== */

/*
** Helper: allocate a fake BlobSpot buffer (no real SQLite connection needed).
*/
static BlobSpot *make_test_blobspot(uint32_t size) {
  BlobSpot *spot = (BlobSpot *)malloc(sizeof(BlobSpot));
  TEST_ASSERT_NOT_NULL(spot);
  memset(spot, 0, sizeof(BlobSpot));
  spot->buffer = (uint8_t *)malloc(size);
  TEST_ASSERT_NOT_NULL(spot->buffer);
  memset(spot->buffer, 0, size);
  spot->buffer_size = size;
  spot->is_writable = 1;
  spot->is_initialized = 1;
  return spot;
}

static void free_test_blobspot(BlobSpot *spot) {
  if (spot) {
    free(spot->buffer);
    free(spot);
  }
}

void test_node_bin_init_and_read_vector(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {1.0f, 2.0f, 3.0f};
  node_bin_init(&idx, spot, 42, vec);

  /* Verify rowid */
  TEST_ASSERT_EQUAL_UINT64(42, read_le64(spot->buffer));

  /* Verify edge count is 0 */
  TEST_ASSERT_EQUAL_UINT16(0, node_bin_edges(&idx, spot));

  /* Verify vector data */
  const float *read_vec = node_bin_vector(&idx, spot);
  TEST_ASSERT_NOT_NULL(read_vec);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, read_vec[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, read_vec[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, read_vec[2]);

  free_test_blobspot(spot);
}

void test_node_bin_add_and_read_edge(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {1.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  /* Add edge to node 2 */
  float edge_vec[3] = {0.0f, 1.0f, 0.0f};
  node_bin_replace_edge(&idx, spot, 0, 2, 1.414f, edge_vec);

  TEST_ASSERT_EQUAL_UINT16(1, node_bin_edges(&idx, spot));

  /* Read edge back */
  uint64_t rowid;
  float dist;
  const float *evec;
  node_bin_edge(&idx, spot, 0, &rowid, &dist, &evec);
  TEST_ASSERT_EQUAL_UINT64(2, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.414f, dist);
  TEST_ASSERT_NOT_NULL(evec);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, evec[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, evec[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, evec[2]);

  free_test_blobspot(spot);
}

void test_node_bin_multiple_edges(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {1.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 100, vec);

  /* Add 3 edges */
  float e1[3] = {0.0f, 1.0f, 0.0f};
  float e2[3] = {0.0f, 0.0f, 1.0f};
  float e3[3] = {1.0f, 1.0f, 0.0f};
  node_bin_replace_edge(&idx, spot, 0, 200, 1.0f, e1);
  node_bin_replace_edge(&idx, spot, 1, 300, 2.0f, e2);
  node_bin_replace_edge(&idx, spot, 2, 400, 1.5f, e3);

  TEST_ASSERT_EQUAL_UINT16(3, node_bin_edges(&idx, spot));

  /* Read all edges back */
  uint64_t rowid;
  float dist;
  node_bin_edge(&idx, spot, 0, &rowid, &dist, NULL);
  TEST_ASSERT_EQUAL_UINT64(200, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, dist);

  node_bin_edge(&idx, spot, 1, &rowid, &dist, NULL);
  TEST_ASSERT_EQUAL_UINT64(300, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, dist);

  node_bin_edge(&idx, spot, 2, &rowid, &dist, NULL);
  TEST_ASSERT_EQUAL_UINT64(400, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, dist);

  free_test_blobspot(spot);
}

void test_node_bin_edge_find_idx(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {0.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e[3] = {1.0f, 1.0f, 1.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e);
  node_bin_replace_edge(&idx, spot, 1, 20, 2.0f, e);
  node_bin_replace_edge(&idx, spot, 2, 30, 3.0f, e);

  TEST_ASSERT_EQUAL_INT(0, node_bin_edge_find_idx(&idx, spot, 10));
  TEST_ASSERT_EQUAL_INT(1, node_bin_edge_find_idx(&idx, spot, 20));
  TEST_ASSERT_EQUAL_INT(2, node_bin_edge_find_idx(&idx, spot, 30));
  TEST_ASSERT_EQUAL_INT(-1, node_bin_edge_find_idx(&idx, spot, 99));

  free_test_blobspot(spot);
}

void test_node_bin_delete_edge(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {0.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e[3] = {1.0f, 1.0f, 1.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e);
  node_bin_replace_edge(&idx, spot, 1, 20, 2.0f, e);
  node_bin_replace_edge(&idx, spot, 2, 30, 3.0f, e);

  /* Delete edge 0 (rowid=10) — last edge (rowid=30) swaps into position 0 */
  node_bin_delete_edge(&idx, spot, 0);
  TEST_ASSERT_EQUAL_UINT16(2, node_bin_edges(&idx, spot));

  /* Edge at position 0 should now be the former last (rowid=30) */
  uint64_t rowid;
  float dist;
  node_bin_edge(&idx, spot, 0, &rowid, &dist, NULL);
  TEST_ASSERT_EQUAL_UINT64(30, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, dist);

  /* Edge at position 1 should still be rowid=20 */
  node_bin_edge(&idx, spot, 1, &rowid, &dist, NULL);
  TEST_ASSERT_EQUAL_UINT64(20, rowid);

  free_test_blobspot(spot);
}

void test_node_bin_delete_last_edge(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {0.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e[3] = {1.0f, 1.0f, 1.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e);
  node_bin_replace_edge(&idx, spot, 1, 20, 2.0f, e);

  /* Delete last edge — just decrements count, no swap needed */
  node_bin_delete_edge(&idx, spot, 1);
  TEST_ASSERT_EQUAL_UINT16(1, node_bin_edges(&idx, spot));

  uint64_t rowid;
  node_bin_edge(&idx, spot, 0, &rowid, NULL, NULL);
  TEST_ASSERT_EQUAL_UINT64(10, rowid);

  free_test_blobspot(spot);
}

void test_node_bin_prune_edges(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {0.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e[3] = {1.0f, 1.0f, 1.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e);
  node_bin_replace_edge(&idx, spot, 1, 20, 2.0f, e);
  node_bin_replace_edge(&idx, spot, 2, 30, 3.0f, e);

  /* Prune to 1 edge */
  node_bin_prune_edges(&idx, spot, 1);
  TEST_ASSERT_EQUAL_UINT16(1, node_bin_edges(&idx, spot));

  /* First edge should still be intact */
  uint64_t rowid;
  node_bin_edge(&idx, spot, 0, &rowid, NULL, NULL);
  TEST_ASSERT_EQUAL_UINT64(10, rowid);

  free_test_blobspot(spot);
}

void test_node_bin_replace_existing_edge(void) {
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {0.0f, 0.0f, 0.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e1[3] = {1.0f, 0.0f, 0.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e1);

  /* Replace edge 0 with new data */
  float e2[3] = {0.0f, 1.0f, 0.0f};
  node_bin_replace_edge(&idx, spot, 0, 99, 5.0f, e2);

  /* Count should still be 1 (replaced, not appended) */
  TEST_ASSERT_EQUAL_UINT16(1, node_bin_edges(&idx, spot));

  uint64_t rowid;
  float dist;
  const float *evec;
  node_bin_edge(&idx, spot, 0, &rowid, &dist, &evec);
  TEST_ASSERT_EQUAL_UINT64(99, rowid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, dist);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, evec[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, evec[1]);

  free_test_blobspot(spot);
}

void test_node_bin_edge_null_outputs(void) {
  /* Verify that passing NULL for optional outputs doesn't crash */
  DiskAnnIndex idx = make_test_index(3, 256);
  BlobSpot *spot = make_test_blobspot(256);

  float vec[3] = {1.0f, 2.0f, 3.0f};
  node_bin_init(&idx, spot, 1, vec);

  float e[3] = {4.0f, 5.0f, 6.0f};
  node_bin_replace_edge(&idx, spot, 0, 10, 1.0f, e);

  /* All NULL — shouldn't crash */
  node_bin_edge(&idx, spot, 0, NULL, NULL, NULL);

  /* Just rowid */
  uint64_t rowid;
  node_bin_edge(&idx, spot, 0, &rowid, NULL, NULL);
  TEST_ASSERT_EQUAL_UINT64(10, rowid);

  /* Just distance */
  float dist;
  node_bin_edge(&idx, spot, 0, NULL, &dist, NULL);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, dist);

  /* Just vector */
  const float *evec;
  node_bin_edge(&idx, spot, 0, NULL, NULL, &evec);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, evec[0]);

  free_test_blobspot(spot);
}

/* ========================================================================
** Distance calculation tests
** ======================================================================== */

void test_distance_l2_orthogonal(void) {
  /* [1,0] and [0,1] — L2 squared distance = 2.0 */
  float a[2] = {1.0f, 0.0f};
  float b[2] = {0.0f, 1.0f};
  float d = diskann_distance_l2(a, b, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, d);
}

void test_distance_l2_same(void) {
  float a[3] = {1.0f, 2.0f, 3.0f};
  float d = diskann_distance_l2(a, a, 3);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, d);
}

void test_distance_l2_known_value(void) {
  /* [3,4] and [0,0] — L2 squared = 9+16 = 25 */
  float a[2] = {3.0f, 4.0f};
  float b[2] = {0.0f, 0.0f};
  float d = diskann_distance_l2(a, b, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, d);
}

void test_distance_cosine_orthogonal(void) {
  /* [1,0] and [0,1] — cosine similarity = 0, distance = 1.0 */
  float a[2] = {1.0f, 0.0f};
  float b[2] = {0.0f, 1.0f};
  float d = diskann_distance_cosine(a, b, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, d);
}

void test_distance_cosine_same_direction(void) {
  /* [1,2,3] and [2,4,6] — same direction, distance = 0 */
  float a[3] = {1.0f, 2.0f, 3.0f};
  float b[3] = {2.0f, 4.0f, 6.0f};
  float d = diskann_distance_cosine(a, b, 3);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, d);
}

void test_distance_cosine_opposite(void) {
  /* [1,0] and [-1,0] — opposite direction, distance = 2.0 */
  float a[2] = {1.0f, 0.0f};
  float b[2] = {-1.0f, 0.0f};
  float d = diskann_distance_cosine(a, b, 2);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, d);
}

void test_distance_dispatch_l2(void) {
  float a[2] = {3.0f, 4.0f};
  float b[2] = {0.0f, 0.0f};
  float d = diskann_distance(a, b, 2, DISKANN_METRIC_EUCLIDEAN);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, d);
}

void test_distance_dispatch_cosine(void) {
  float a[2] = {1.0f, 0.0f};
  float b[2] = {0.0f, 1.0f};
  float d = diskann_distance(a, b, 2, DISKANN_METRIC_COSINE);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, d);
}

/* ========================================================================
** Buffer management tests
** ======================================================================== */

void test_distance_buffer_insert_idx_empty(void) {
  float distances[4] = {0};
  int idx = distance_buffer_insert_idx(distances, 0, 4, 5.0f);
  TEST_ASSERT_EQUAL_INT(0, idx);
}

void test_distance_buffer_insert_idx_beginning(void) {
  float distances[4] = {2.0f, 4.0f, 6.0f};
  int idx = distance_buffer_insert_idx(distances, 3, 4, 1.0f);
  TEST_ASSERT_EQUAL_INT(0, idx);
}

void test_distance_buffer_insert_idx_middle(void) {
  float distances[4] = {2.0f, 4.0f, 6.0f};
  int idx = distance_buffer_insert_idx(distances, 3, 4, 3.0f);
  TEST_ASSERT_EQUAL_INT(1, idx);
}

void test_distance_buffer_insert_idx_end(void) {
  float distances[4] = {2.0f, 4.0f, 6.0f};
  int idx = distance_buffer_insert_idx(distances, 3, 4, 7.0f);
  TEST_ASSERT_EQUAL_INT(3, idx); /* There's room (size=3, max=4) */
}

void test_distance_buffer_insert_idx_full_rejected(void) {
  float distances[3] = {2.0f, 4.0f, 6.0f};
  int idx = distance_buffer_insert_idx(distances, 3, 3, 7.0f);
  TEST_ASSERT_EQUAL_INT(-1, idx); /* Full, and larger than all */
}

void test_buffer_insert_basic(void) {
  int32_t buf[4] = {10, 30, 0, 0};
  int32_t item = 20;
  buffer_insert((uint8_t *)buf, 2, 4, 1, sizeof(int32_t), (uint8_t *)&item,
                NULL);
  TEST_ASSERT_EQUAL_INT32(10, buf[0]);
  TEST_ASSERT_EQUAL_INT32(20, buf[1]);
  TEST_ASSERT_EQUAL_INT32(30, buf[2]);
}

void test_buffer_insert_evicts_last(void) {
  int32_t buf[3] = {10, 20, 30};
  int32_t item = 15;
  int32_t last = 0;
  buffer_insert((uint8_t *)buf, 3, 3, 1, sizeof(int32_t), (uint8_t *)&item,
                (uint8_t *)&last);
  TEST_ASSERT_EQUAL_INT32(10, buf[0]);
  TEST_ASSERT_EQUAL_INT32(15, buf[1]);
  TEST_ASSERT_EQUAL_INT32(20, buf[2]);
  TEST_ASSERT_EQUAL_INT32(30, last); /* Evicted item */
}

void test_buffer_delete_basic(void) {
  int32_t buf[3] = {10, 20, 30};
  buffer_delete((uint8_t *)buf, 3, 1, sizeof(int32_t));
  TEST_ASSERT_EQUAL_INT32(10, buf[0]);
  TEST_ASSERT_EQUAL_INT32(30, buf[1]);
}

void test_buffer_delete_first(void) {
  int32_t buf[3] = {10, 20, 30};
  buffer_delete((uint8_t *)buf, 3, 0, sizeof(int32_t));
  TEST_ASSERT_EQUAL_INT32(20, buf[0]);
  TEST_ASSERT_EQUAL_INT32(30, buf[1]);
}

void test_buffer_delete_last(void) {
  int32_t buf[3] = {10, 20, 30};
  buffer_delete((uint8_t *)buf, 3, 2, sizeof(int32_t));
  /* First two unchanged — last element "deleted" by reducing size */
  TEST_ASSERT_EQUAL_INT32(10, buf[0]);
  TEST_ASSERT_EQUAL_INT32(20, buf[1]);
}

/* ========================================================================
** Node alloc/free tests
** ======================================================================== */

void test_node_alloc_basic(void) {
  DiskAnnNode *node = diskann_node_alloc(42);
  TEST_ASSERT_NOT_NULL(node);
  TEST_ASSERT_EQUAL_UINT64(42, node->rowid);
  TEST_ASSERT_EQUAL_INT(0, node->visited);
  TEST_ASSERT_NULL(node->next);
  TEST_ASSERT_NULL(node->blob_spot);
  diskann_node_free(node);
}

void test_node_free_null(void) {
  /* Should not crash */
  diskann_node_free(NULL);
}

/* ========================================================================
** DiskAnnIndex derived fields tests
** ======================================================================== */

void test_open_index_computes_derived_fields(void) {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  DiskAnnConfig config = {.dimensions = 128,
                          .metric = DISKANN_METRIC_COSINE,
                          .max_neighbors = 16,
                          .search_list_size = 50,
                          .insert_list_size = 100,
                          .block_size = 0};

  rc = diskann_create_index(db, "main", "test_idx", &config);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  DiskAnnIndex *idx;
  rc = diskann_open_index(db, "main", "test_idx", &idx);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Verify derived fields */
  TEST_ASSERT_EQUAL_UINT32(128 * sizeof(float), idx->nNodeVectorSize);
  TEST_ASSERT_EQUAL_UINT32(idx->nNodeVectorSize, idx->nEdgeVectorSize);
  /* pruning_alpha default changed from 1.2 to 1.4 for better connectivity */
  TEST_ASSERT_FLOAT_WITHIN(0.01, 1.4, idx->pruning_alpha);

  diskann_close_index(idx);
  sqlite3_close(db);
}
