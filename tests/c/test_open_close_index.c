/*
** Tests for diskann_open_index() and diskann_close_index()
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
** MIT License
*/
#include "../../src/diskann.h"
#include "unity/unity.h"
#include <sqlite3.h>

/*
** Helper: Create a test index for opening
*/
static int create_test_index(sqlite3 *db, const char *name) {
  DiskAnnConfig config = {.dimensions = 128,
                          .metric = DISKANN_METRIC_EUCLIDEAN,
                          .max_neighbors = 32,
                          .search_list_size = 100,
                          .insert_list_size = 200,
                          .block_size = 4096};
  return diskann_create_index(db, "main", name, &config);
}

/*
** Test opening an existing index with valid parameters
*/
void test_open_index_with_valid_params(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create index first */
  rc = create_test_index(db, "test_idx");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Now open it */
  rc = diskann_open_index(db, "main", "test_idx", &idx);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx);

  /* Clean up */
  diskann_close_index(idx);
  sqlite3_close(db);
}

/*
** Test opening non-existent index (should fail)
*/
void test_open_index_not_found(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Try to open non-existent index */
  rc = diskann_open_index(db, "main", "nonexistent", &idx);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);
  TEST_ASSERT_NULL(idx);

  sqlite3_close(db);
}

/*
** Test opening index with NULL database (should fail)
*/
void test_open_index_null_database(void) {
  DiskAnnIndex *idx = NULL;
  int rc = diskann_open_index(NULL, "main", "test_idx", &idx);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
  TEST_ASSERT_NULL(idx);
}

/*
** Test opening index with NULL index name (should fail)
*/
void test_open_index_null_name(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  rc = diskann_open_index(db, "main", NULL, &idx);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
  TEST_ASSERT_NULL(idx);

  sqlite3_close(db);
}

/*
** Test opening index with NULL output pointer (should fail)
*/
void test_open_index_null_output(void) {
  sqlite3 *db = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  rc = create_test_index(db, "test_idx");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* NULL output pointer should fail */
  rc = diskann_open_index(db, "main", "test_idx", NULL);
  TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

  sqlite3_close(db);
}

/*
** Test closing NULL index (should be safe)
*/
void test_close_index_null(void) {
  /* Should not crash */
  diskann_close_index(NULL);
  TEST_PASS();
}

/*
** Test closing index frees resources properly
** (Valgrind will catch leaks)
*/
void test_close_index_frees_resources(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  rc = create_test_index(db, "test_idx");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  rc = diskann_open_index(db, "main", "test_idx", &idx);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx);

  /* Close should free all resources - Valgrind will verify */
  diskann_close_index(idx);
  /* After close, don't use idx - it's been freed */

  sqlite3_close(db);
}

/*
** Test opening multiple indexes simultaneously
*/
void test_open_multiple_indexes(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx1 = NULL;
  DiskAnnIndex *idx2 = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create two indexes */
  rc = create_test_index(db, "idx1");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  rc = create_test_index(db, "idx2");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Open both */
  rc = diskann_open_index(db, "main", "idx1", &idx1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx1);

  rc = diskann_open_index(db, "main", "idx2", &idx2);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx2);

  /* Verify they're different */
  TEST_ASSERT_NOT_EQUAL(idx1, idx2);

  /* Close both */
  diskann_close_index(idx1);
  diskann_close_index(idx2);

  sqlite3_close(db);
}

/*
** Test re-opening the same index multiple times
*/
void test_reopen_same_index(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx1 = NULL;
  DiskAnnIndex *idx2 = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  rc = create_test_index(db, "test_idx");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Open first time */
  rc = diskann_open_index(db, "main", "test_idx", &idx1);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx1);

  /* Close it */
  diskann_close_index(idx1);

  /* Open again - should work */
  rc = diskann_open_index(db, "main", "test_idx", &idx2);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_NOT_NULL(idx2);

  diskann_close_index(idx2);
  sqlite3_close(db);
}

/*
** Test that open_index rejects corrupted metadata with out-of-bounds dimensions
*/
void test_open_index_rejects_huge_dimensions(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create a valid index first */
  rc = create_test_index(db, "corrupt_dim");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Corrupt the metadata: set dimensions to an absurdly large value */
  char *err = NULL;
  rc = sqlite3_exec(
      db,
      "UPDATE corrupt_dim_metadata SET value = 999999 WHERE key = 'dimensions'",
      NULL, NULL, &err);
  if (err)
    sqlite3_free(err);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Open should fail due to out-of-bounds dimensions */
  rc = diskann_open_index(db, "main", "corrupt_dim", &idx);
  TEST_ASSERT_EQUAL(DISKANN_ERROR, rc);
  TEST_ASSERT_NULL(idx);

  sqlite3_close(db);
}

/*
** Test that open_index rejects corrupted metadata with out-of-bounds block_size
*/
void test_open_index_rejects_huge_block_size(void) {
  sqlite3 *db = NULL;
  DiskAnnIndex *idx = NULL;
  int rc;

  rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Create a valid index first */
  rc = create_test_index(db, "corrupt_bs");
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);

  /* Corrupt the metadata: set block_size beyond MAX_BLOCK_SIZE (128MB) */
  char *err = NULL;
  rc = sqlite3_exec(db,
                    "UPDATE corrupt_bs_metadata SET value = 999999999 WHERE "
                    "key = 'block_size'",
                    NULL, NULL, &err);
  if (err)
    sqlite3_free(err);
  TEST_ASSERT_EQUAL(SQLITE_OK, rc);

  /* Open should fail due to out-of-bounds block_size */
  rc = diskann_open_index(db, "main", "corrupt_bs", &idx);
  TEST_ASSERT_EQUAL(DISKANN_ERROR, rc);
  TEST_ASSERT_NULL(idx);

  sqlite3_close(db);
}

/* main() is in test_runner.c */
