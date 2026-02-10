/*
** Basic tests for DiskANN public API
** These tests just verify we can reference the functions
*/
#include "unity/unity.h"
#include "../../src/diskann.h"
#include <sqlite3.h>

/* setUp/tearDown in test_runner.c */

/*
** Test that we can reference diskann_create_index function
*/
void test_diskann_create_index_exists(void) {
    /* Just verify the function exists and can be called */
    sqlite3 *db = NULL;
    DiskAnnConfig config = {
        .dimensions = 128,
        .metric = DISKANN_METRIC_EUCLIDEAN,
        .max_neighbors = 32,
        .search_list_size = 100,
        .insert_list_size = 200,
        .block_size = 4096
    };

    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = diskann_create_index(db, "main", "test_idx", &config);
    /* We expect this to fail for now since it's not implemented */

    if (db) sqlite3_close(db);
}

/*
** Test that we can reference diskann_open_index function
*/
void test_diskann_open_index_exists(void) {
    DiskAnnIndex *idx = NULL;
    sqlite3 *db = NULL;

    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = diskann_open_index(db, "main", "test_idx", &idx);
    /* We expect this to fail for now since it's not implemented */

    if (db) sqlite3_close(db);
}

/*
** Test that we can reference diskann_insert function
*/
void test_diskann_insert_exists(void) {
    DiskAnnIndex *idx = NULL;
    float vector[128] = {0};

    int rc = diskann_insert(idx, 1, vector, 128);
    (void)rc; /* We expect this to fail for now since idx is NULL */
}

/*
** Test that we can reference diskann_search function
*/
void test_diskann_search_exists(void) {
    DiskAnnIndex *idx = NULL;
    float query[128] = {0};
    DiskAnnResult results[10];

    int rc = diskann_search(idx, query, 128, 10, results);
    (void)rc; /* We expect this to fail for now since idx is NULL */
}

/*
** Test that we can reference diskann_close_index function
*/
void test_diskann_close_index_exists(void) {
    /* Should handle NULL gracefully */
    diskann_close_index(NULL);
    TEST_ASSERT(1); /* If we get here, function exists */
}

/*
** Test that we can reference diskann_delete function
*/
void test_diskann_delete_exists(void) {
    DiskAnnIndex *idx = NULL;

    int rc = diskann_delete(idx, 1);
    (void)rc; /* We expect this to fail for now */
}

/*
** Test that we can reference diskann_drop_index function
*/
void test_diskann_drop_index_exists(void) {
    sqlite3 *db = NULL;

    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = diskann_drop_index(db, "main", "test_idx");
    /* We expect this to fail for now */

    if (db) sqlite3_close(db);
}

/*
** Test that we can reference diskann_clear_index function
*/
void test_diskann_clear_index_exists(void) {
    sqlite3 *db = NULL;

    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = diskann_clear_index(db, "main", "test_idx");
    /* We expect this to fail for now */

    if (db) sqlite3_close(db);
}

/* main() in test_runner.c */
