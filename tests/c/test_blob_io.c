/*
** Tests for BLOB I/O layer
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "../../src/diskann.h"
#include "../../src/diskann_blob.h"
#include "../../src/diskann_internal.h"
#include "unity/unity.h"
#include <sqlite3.h>
#include <string.h>

/* Helper: Create test index and return opened index handle */
static DiskAnnIndex *create_and_open_test_index(sqlite3 *db, const char *name) {
  DiskAnnConfig config = {.dimensions = 128,
                          .metric = DISKANN_METRIC_EUCLIDEAN,
                          .max_neighbors = 32,
                          .search_list_size = 100,
                          .insert_list_size = 200,
                          .block_size = 4096};

  int rc = diskann_create_index(db, "main", name, &config);
  if (rc != DISKANN_OK)
    return NULL;

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", name, &idx);
  if (rc != DISKANN_OK)
    return NULL;

  return idx;
}

/* Helper: Insert a test row into shadow table */
static int insert_test_row(sqlite3 *db, const char *table, int64_t id,
                           const uint8_t *data, size_t size) {
  char *sql =
      sqlite3_mprintf("INSERT INTO %s_shadow (id, data) VALUES (?, ?)", table);
  if (!sql)
    return SQLITE_NOMEM;

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK)
    return rc;

  sqlite3_bind_int64(stmt, 1, id);
  sqlite3_bind_blob(stmt, 2, data, (int)size, SQLITE_STATIC);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

/*
** Test creating a BlobSpot for an existing row
*/
void test_blob_spot_create_existing_row(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert a test row with 4KB of data */
  uint8_t test_data[4096] = {0};
  memset(test_data, 0xAB, sizeof(test_data));
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create BlobSpot for the row */
  rc = blob_spot_create(idx, &spot, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(spot);
  TEST_ASSERT_EQUAL(1, spot->rowid);
  TEST_ASSERT_EQUAL(4096, spot->buffer_size);
  TEST_ASSERT_EQUAL(0, spot->is_writable);

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test creating BlobSpot for non-existent row (should fail)
*/
void test_blob_spot_create_nonexistent_row(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Try to create BlobSpot for non-existent row */
  rc = blob_spot_create(idx, &spot, 999, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_ROW_NOT_FOUND, rc);
  TEST_ASSERT_NULL(spot);

  /* Cleanup */
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test creating writable BlobSpot
*/
void test_blob_spot_create_writable(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert a test row */
  uint8_t test_data[4096] = {0};
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create writable BlobSpot */
  rc = blob_spot_create(idx, &spot, 1, 4096, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(spot);
  TEST_ASSERT_EQUAL(1, spot->is_writable);

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test freeing NULL BlobSpot (should be safe)
*/
void test_blob_spot_free_null(void) {
  blob_spot_free(NULL);
  TEST_PASS();
}

/*
** Test reloading BlobSpot with same rowid (should be no-op)
*/
void test_blob_spot_reload_same_rowid(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert test row */
  uint8_t test_data[4096] = {0};
  memset(test_data, 0xCD, sizeof(test_data));
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create and reload */
  rc = blob_spot_create(idx, &spot, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* First reload should read data */
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(1, spot->is_initialized);
  TEST_ASSERT_EQUAL(0xCD, spot->buffer[0]);

  /* Second reload with same rowid should be no-op */
  uint64_t old_reads = idx->num_reads;
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(old_reads, idx->num_reads); /* No additional read */

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test reloading BlobSpot with different rowid
*/
void test_blob_spot_reload_different_rowid(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert two test rows */
  uint8_t test_data1[4096], test_data2[4096];
  memset(test_data1, 0x11, sizeof(test_data1));
  memset(test_data2, 0x22, sizeof(test_data2));
  rc = insert_test_row(db, "test_idx", 1, test_data1, sizeof(test_data1));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);
  rc = insert_test_row(db, "test_idx", 2, test_data2, sizeof(test_data2));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create BlobSpot for row 1 */
  rc = blob_spot_create(idx, &spot, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Load row 1 */
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(0x11, spot->buffer[0]);

  /* Reload to row 2 - should use sqlite3_blob_reopen */
  rc = blob_spot_reload(idx, spot, 2, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(2, spot->rowid);
  TEST_ASSERT_EQUAL(0x22, spot->buffer[0]);

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test flushing BlobSpot (writing buffer to database)
*/
void test_blob_spot_flush(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert test row with zeroes */
  uint8_t test_data[4096] = {0};
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create writable BlobSpot and load data */
  rc = blob_spot_create(idx, &spot, 1, 4096, 1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Modify buffer */
  memset(spot->buffer, 0xFF, spot->buffer_size);

  /* Flush to database */
  rc = blob_spot_flush(idx, spot);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_TRUE(idx->num_writes > 0);

  /* Close and reopen to verify data was written */
  blob_spot_free(spot);
  spot = NULL;

  rc = blob_spot_create(idx, &spot, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Verify data */
  TEST_ASSERT_EQUAL(0xFF, spot->buffer[0]);
  TEST_ASSERT_EQUAL(0xFF, spot->buffer[4095]);

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test flushing read-only BlobSpot (should fail)
*/
void test_blob_spot_flush_readonly(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  BlobSpot *spot = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert test row */
  uint8_t test_data[4096] = {0};
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create read-only BlobSpot */
  rc = blob_spot_create(idx, &spot, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(0, spot->is_writable);

  /* Load data */
  rc = blob_spot_reload(idx, spot, 1, 4096);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Try to flush - should fail because read-only */
  rc = blob_spot_flush(idx, spot);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

  /* Cleanup */
  blob_spot_free(spot);
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test blob_spot_create with NULL output pointer (should fail)
*/
void test_blob_spot_create_null_output(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;

  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  idx = create_and_open_test_index(db, "test_idx");
  TEST_ASSERT_NOT_NULL(idx);

  /* Insert test row */
  uint8_t test_data[4096] = {0};
  rc = insert_test_row(db, "test_idx", 1, test_data, sizeof(test_data));
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Try to create with NULL output pointer - should fail */
  rc = blob_spot_create(idx, NULL, 1, 4096, 0);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

  /* Cleanup */
  diskann_close_index(idx);
  sqlite3_close(db);
}

/* main() is in test_runner.c */
