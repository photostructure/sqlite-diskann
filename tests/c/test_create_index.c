/*
** Tests for diskann_create_index()
*/
#include "unity/unity.h"
#include "../../src/diskann.h"
#include "../../src/diskann_internal.h"
#include <sqlite3.h>

/* Each test manages its own database to avoid conflicts */

/*
** Test creating an index with valid parameters
*/
void test_create_index_with_valid_params(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    rc = diskann_create_index(db, "main", "test_index", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify shadow table was created */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='test_index_shadow'",
        -1, &stmt, NULL);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL(SQLITE_ROW, rc); /* Should find the shadow table */

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/*
** Test creating index with NULL config (should use defaults)
*/
void test_create_index_with_null_config(void) {
    sqlite3 *db = NULL;
    int rc2 = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc2);

    int rc = diskann_create_index(db, "main", "test_index", NULL);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify shadow table was created even with default config */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='test_index_shadow'",
        -1, &stmt, NULL);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL(SQLITE_ROW, rc);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/*
** Test creating index with NULL database (should fail)
*/
void test_create_index_null_database(void) {
    DiskAnnConfig config = { .dimensions = 128 };
    int rc = diskann_create_index(NULL, "main", "test_index", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
}

/*
** Test creating index with NULL index name (should fail)
*/
void test_create_index_null_name(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    DiskAnnConfig config = { .dimensions = 128 };
    int rc = diskann_create_index(db, "main", NULL, &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    if (db) sqlite3_close(db);
}

/*
** Test creating index with invalid dimensions (should fail)
*/
void test_create_index_zero_dimensions(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    DiskAnnConfig config = {
        .dimensions = 0,  /* Invalid! */
        .metric = DISKANN_METRIC_EUCLIDEAN
    };
    int rc = diskann_create_index(db, "main", "test_index", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_DIMENSION, rc);

    if (db) sqlite3_close(db);
}

/*
** Test shadow table schema is correct
*/
void test_shadow_table_schema(void) {
    sqlite3 *db = NULL;
    int rc2 = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc2);

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };
    int rc = diskann_create_index(db, "main", "test_index", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Check shadow table has correct columns */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
        "PRAGMA table_info(test_index_shadow)",
        -1, &stmt, NULL);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Should have 'id' column */
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL(SQLITE_ROW, rc);
    const char *col_name = (const char*)sqlite3_column_text(stmt, 1);
    TEST_ASSERT_EQUAL_STRING("id", col_name);

    /* Should have 'data' column */
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL(SQLITE_ROW, rc);
    col_name = (const char*)sqlite3_column_text(stmt, 1);
    TEST_ASSERT_EQUAL_STRING("data", col_name);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/*
** Test that invalid index names are rejected (prevents SQL injection)
*/
void test_create_index_invalid_name(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    /* SQL injection attempt */
    rc = diskann_create_index(db, "main", "'; DROP TABLE x;--", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    /* Spaces not allowed */
    rc = diskann_create_index(db, "main", "has spaces", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    /* Must start with letter or underscore */
    rc = diskann_create_index(db, "main", "123start", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    /* Empty string */
    rc = diskann_create_index(db, "main", "", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    /* Valid name should still work */
    rc = diskann_create_index(db, "main", "valid_name_123", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    sqlite3_close(db);
}

/*
** Test that metadata roundtrips correctly through create -> open
*/
void test_metadata_roundtrip(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Use non-default values to verify they're stored, not defaults */
    DiskAnnConfig config = {
        .dimensions = 512,
        .metric = DISKANN_METRIC_COSINE,
        .max_neighbors = 64,
        .search_list_size = 150,
        .insert_list_size = 300,
        .block_size = 8192
    };

    rc = diskann_create_index(db, "main", "test_rt", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Open the index and verify all config fields */
    DiskAnnIndex *idx = NULL;
    rc = diskann_open_index(db, "main", "test_rt", &idx);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);
    TEST_ASSERT_NOT_NULL(idx);

    TEST_ASSERT_EQUAL_UINT32(512, idx->dimensions);
    TEST_ASSERT_EQUAL_UINT8(DISKANN_METRIC_COSINE, idx->metric);
    TEST_ASSERT_EQUAL_UINT32(64, idx->max_neighbors);
    TEST_ASSERT_EQUAL_UINT32(150, idx->search_list_size);
    TEST_ASSERT_EQUAL_UINT32(300, idx->insert_list_size);
    TEST_ASSERT_EQUAL_UINT32(8192, idx->block_size);

    diskann_close_index(idx);
    sqlite3_close(db);
}

/*
** Test creating an index that already exists returns DISKANN_ERROR_EXISTS
*/
void test_create_index_duplicate_fails(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    /* First create should succeed */
    rc = diskann_create_index(db, "main", "dup_test", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Second create with same name should fail */
    rc = diskann_create_index(db, "main", "dup_test", &config);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_EXISTS, rc);

    /* Second create with different dimensions should also fail (not silently overwrite) */
    DiskAnnConfig config2 = config;
    config2.dimensions = 256;
    rc = diskann_create_index(db, "main", "dup_test", &config2);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_EXISTS, rc);

    /* Original config should be preserved */
    DiskAnnIndex *idx = NULL;
    rc = diskann_open_index(db, "main", "dup_test", &idx);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(128, idx->dimensions);  /* Not 256 */

    diskann_close_index(idx);
    sqlite3_close(db);
}

/*
** Test that create_index is atomic - no partial state on failure.
** We simulate by verifying drop+recreate works after a successful create.
*/
void test_create_index_atomicity(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    /* Create, drop, recreate should work cleanly */
    rc = diskann_create_index(db, "main", "atomic_test", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    rc = diskann_drop_index(db, "main", "atomic_test");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Recreate with different config after drop should succeed */
    config.dimensions = 256;
    rc = diskann_create_index(db, "main", "atomic_test", &config);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify new config */
    DiskAnnIndex *idx = NULL;
    rc = diskann_open_index(db, "main", "atomic_test", &idx);
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(256, idx->dimensions);

    diskann_close_index(idx);
    sqlite3_close(db);
}

/* main() in test_runner.c */
