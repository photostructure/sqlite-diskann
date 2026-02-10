/*
** Tests for diskann_drop_index() and diskann_clear_index()
*/
#include "unity/unity.h"
#include "../../src/diskann.h"
#include <sqlite3.h>

/*
** Helper: Create a test index for drop/clear operations
*/
static sqlite3* create_test_db_with_index(const char *index_name) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) return NULL;

    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    rc = diskann_create_index(db, "main", index_name, &config);
    if (rc != DISKANN_OK) {
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

/*
** Helper: Check if shadow table exists
** Uses %w quoting for proper SQL identifier escaping.
*/
static int shadow_table_exists(sqlite3 *db, const char *index_name) {
    char *tbl_name = sqlite3_mprintf("%s_shadow", index_name);
    if (!tbl_name) return 0;

    char *sql = sqlite3_mprintf(
        "SELECT name FROM sqlite_master WHERE type='table' AND name=%Q",
        tbl_name
    );
    sqlite3_free(tbl_name);
    if (!sql) return 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_ROW) ? 1 : 0;
}

/*
** Helper: Get row count from shadow table
** Uses %w quoting for proper SQL identifier escaping.
*/
static int get_shadow_table_row_count(sqlite3 *db, const char *index_name) {
    char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM \"%w_shadow\"", index_name);
    if (!sql) return -1;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);

    return count;
}

/*
** Test dropping an index removes the shadow table
*/
void test_drop_index_removes_shadow_table(void) {
    sqlite3 *db = create_test_db_with_index("test_drop");
    TEST_ASSERT_NOT_NULL(db);

    /* Verify shadow table exists before drop */
    TEST_ASSERT_EQUAL(1, shadow_table_exists(db, "test_drop"));

    /* Drop the index */
    int rc = diskann_drop_index(db, "main", "test_drop");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify shadow table no longer exists */
    TEST_ASSERT_EQUAL(0, shadow_table_exists(db, "test_drop"));

    sqlite3_close(db);
}

/*
** Test dropping non-existent index fails gracefully
*/
void test_drop_nonexistent_index(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Try to drop non-existent index - should fail */
    rc = diskann_drop_index(db, "main", "nonexistent");
    TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

    sqlite3_close(db);
}

/*
** Test drop with NULL database fails
*/
void test_drop_index_null_database(void) {
    int rc = diskann_drop_index(NULL, "main", "test_index");
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
}

/*
** Test drop with NULL index name fails
*/
void test_drop_index_null_name(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    int rc = diskann_drop_index(db, "main", NULL);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    if (db) sqlite3_close(db);
}

/*
** Test clearing an index removes data but keeps table
*/
void test_clear_index_removes_data_keeps_table(void) {
    sqlite3 *db = create_test_db_with_index("test_clear");
    TEST_ASSERT_NOT_NULL(db);

    /* Insert some test data into shadow table */
    char *sql = sqlite3_mprintf(
        "INSERT INTO test_clear_shadow (id, data) VALUES (1, zeroblob(4096))"
    );
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    sqlite3_free(sql);
    if (err_msg) sqlite3_free(err_msg);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Verify data exists */
    TEST_ASSERT_EQUAL(1, get_shadow_table_row_count(db, "test_clear"));

    /* Clear the index */
    rc = diskann_clear_index(db, "main", "test_clear");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify shadow table still exists */
    TEST_ASSERT_EQUAL(1, shadow_table_exists(db, "test_clear"));

    /* Verify data was deleted */
    TEST_ASSERT_EQUAL(0, get_shadow_table_row_count(db, "test_clear"));

    sqlite3_close(db);
}

/*
** Test clearing non-existent index fails
*/
void test_clear_nonexistent_index(void) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Try to clear non-existent index - should fail */
    rc = diskann_clear_index(db, "main", "nonexistent");
    TEST_ASSERT_EQUAL(DISKANN_ERROR_NOTFOUND, rc);

    sqlite3_close(db);
}

/*
** Test clear with NULL database fails
*/
void test_clear_index_null_database(void) {
    int rc = diskann_clear_index(NULL, "main", "test_index");
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);
}

/*
** Test clear with NULL index name fails
*/
void test_clear_index_null_name(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    int rc = diskann_clear_index(db, "main", NULL);
    TEST_ASSERT_EQUAL(DISKANN_ERROR_INVALID, rc);

    if (db) sqlite3_close(db);
}

/*
** Test clearing empty index succeeds
*/
void test_clear_empty_index(void) {
    sqlite3 *db = create_test_db_with_index("test_empty");
    TEST_ASSERT_NOT_NULL(db);

    /* Index already empty, clearing should succeed */
    int rc = diskann_clear_index(db, "main", "test_empty");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify table still exists and is empty */
    TEST_ASSERT_EQUAL(1, shadow_table_exists(db, "test_empty"));
    TEST_ASSERT_EQUAL(0, get_shadow_table_row_count(db, "test_empty"));

    sqlite3_close(db);
}

/*
** Test drop after clear works
*/
void test_drop_after_clear(void) {
    sqlite3 *db = create_test_db_with_index("test_combo");
    TEST_ASSERT_NOT_NULL(db);

    /* Clear first */
    int rc = diskann_clear_index(db, "main", "test_combo");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Then drop */
    rc = diskann_drop_index(db, "main", "test_combo");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify table is gone */
    TEST_ASSERT_EQUAL(0, shadow_table_exists(db, "test_combo"));

    sqlite3_close(db);
}

/*
** Helper: Check if metadata table exists
*/
static int metadata_table_exists(sqlite3 *db, const char *index_name) {
    char *tbl_name = sqlite3_mprintf("%s_metadata", index_name);
    if (!tbl_name) return 0;

    char *sql = sqlite3_mprintf(
        "SELECT name FROM sqlite_master WHERE type='table' AND name=%Q",
        tbl_name
    );
    sqlite3_free(tbl_name);
    if (!sql) return 0;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return 0;

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_ROW) ? 1 : 0;
}

/*
** Helper: Get metadata value from metadata table
*/
static int64_t get_metadata_value(sqlite3 *db, const char *index_name, const char *key) {
    char *sql = sqlite3_mprintf(
        "SELECT value FROM %s_metadata WHERE key = %Q",
        index_name, key
    );
    if (!sql) return -1;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);

    if (rc != SQLITE_OK) return -1;

    rc = sqlite3_step(stmt);
    int64_t value = (rc == SQLITE_ROW) ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);

    return value;
}

/*
** Test clearing index preserves metadata
*/
void test_clear_index_preserves_metadata(void) {
    sqlite3 *db = create_test_db_with_index("test_preserve");
    TEST_ASSERT_NOT_NULL(db);

    /* Verify metadata table exists */
    TEST_ASSERT_EQUAL(1, metadata_table_exists(db, "test_preserve"));

    /* Verify some metadata values exist (from create) */
    int64_t dimensions = get_metadata_value(db, "test_preserve", "dimensions");
    TEST_ASSERT_EQUAL(128, dimensions);

    int64_t max_neighbors = get_metadata_value(db, "test_preserve", "max_neighbors");
    TEST_ASSERT_EQUAL(32, max_neighbors);

    /* Insert some test data into shadow table */
    char *sql = sqlite3_mprintf(
        "INSERT INTO test_preserve_shadow (id, data) VALUES (1, zeroblob(4096))"
    );
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    sqlite3_free(sql);
    if (err_msg) sqlite3_free(err_msg);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    /* Verify data exists in shadow table */
    TEST_ASSERT_EQUAL(1, get_shadow_table_row_count(db, "test_preserve"));

    /* Clear the index */
    rc = diskann_clear_index(db, "main", "test_preserve");
    TEST_ASSERT_EQUAL(DISKANN_OK, rc);

    /* Verify shadow table data was deleted */
    TEST_ASSERT_EQUAL(0, get_shadow_table_row_count(db, "test_preserve"));

    /* Verify metadata table still exists */
    TEST_ASSERT_EQUAL(1, metadata_table_exists(db, "test_preserve"));

    /* Verify metadata values are still intact */
    dimensions = get_metadata_value(db, "test_preserve", "dimensions");
    TEST_ASSERT_EQUAL(128, dimensions);

    max_neighbors = get_metadata_value(db, "test_preserve", "max_neighbors");
    TEST_ASSERT_EQUAL(32, max_neighbors);

    sqlite3_close(db);
}

/* main() in test_runner.c */
