/*
** Main test runner - runs all test suites
*/
#include "unity/unity.h"

/* Forward declarations of test functions */
extern void test_diskann_create_index_exists(void);
extern void test_diskann_open_index_exists(void);
extern void test_diskann_close_index_exists(void);
extern void test_diskann_drop_index_exists(void);
extern void test_diskann_clear_index_exists(void);

extern void test_create_index_with_valid_params(void);
extern void test_create_index_with_null_config(void);
extern void test_create_index_null_database(void);
extern void test_create_index_null_name(void);
extern void test_create_index_zero_dimensions(void);
extern void test_shadow_table_schema(void);
extern void test_create_index_invalid_name(void);
extern void test_metadata_roundtrip(void);
extern void test_create_index_duplicate_fails(void);
extern void test_create_index_atomicity(void);

extern void test_drop_index_removes_shadow_table(void);
extern void test_drop_nonexistent_index(void);
extern void test_drop_index_null_database(void);
extern void test_drop_index_null_name(void);
extern void test_clear_index_removes_data_keeps_table(void);
extern void test_clear_nonexistent_index(void);
extern void test_clear_index_null_database(void);
extern void test_clear_index_null_name(void);
extern void test_clear_empty_index(void);
extern void test_drop_after_clear(void);
extern void test_clear_index_preserves_metadata(void);

/* Open/close index tests */
extern void test_open_index_with_valid_params(void);
extern void test_open_index_not_found(void);
extern void test_open_index_null_database(void);
extern void test_open_index_null_name(void);
extern void test_open_index_null_output(void);
extern void test_close_index_null(void);
extern void test_close_index_frees_resources(void);
extern void test_open_multiple_indexes(void);
extern void test_reopen_same_index(void);
extern void test_open_index_rejects_huge_dimensions(void);
extern void test_open_index_rejects_huge_block_size(void);

/* BLOB I/O tests */
extern void test_blob_spot_create_existing_row(void);
extern void test_blob_spot_create_nonexistent_row(void);
extern void test_blob_spot_create_writable(void);
extern void test_blob_spot_free_null(void);
extern void test_blob_spot_reload_same_rowid(void);
extern void test_blob_spot_reload_different_rowid(void);
extern void test_blob_spot_flush(void);
extern void test_blob_spot_flush_readonly(void);
extern void test_blob_spot_create_null_output(void);

void setUp(void) {
    /* Global setup if needed */
}

void tearDown(void) {
    /* Global teardown if needed */
}

int main(void) {
    UNITY_BEGIN();

    /* API existence tests */
    RUN_TEST(test_diskann_create_index_exists);
    RUN_TEST(test_diskann_open_index_exists);
    RUN_TEST(test_diskann_close_index_exists);
    RUN_TEST(test_diskann_drop_index_exists);
    RUN_TEST(test_diskann_clear_index_exists);

    /* Create index tests */
    RUN_TEST(test_create_index_with_valid_params);
    RUN_TEST(test_create_index_with_null_config);
    RUN_TEST(test_create_index_null_database);
    RUN_TEST(test_create_index_null_name);
    RUN_TEST(test_create_index_zero_dimensions);
    RUN_TEST(test_shadow_table_schema);
    RUN_TEST(test_create_index_invalid_name);
    RUN_TEST(test_metadata_roundtrip);
    RUN_TEST(test_create_index_duplicate_fails);
    RUN_TEST(test_create_index_atomicity);

    /* Drop index tests */
    RUN_TEST(test_drop_index_removes_shadow_table);
    RUN_TEST(test_drop_nonexistent_index);
    RUN_TEST(test_drop_index_null_database);
    RUN_TEST(test_drop_index_null_name);

    /* Clear index tests */
    RUN_TEST(test_clear_index_removes_data_keeps_table);
    RUN_TEST(test_clear_nonexistent_index);
    RUN_TEST(test_clear_index_null_database);
    RUN_TEST(test_clear_index_null_name);
    RUN_TEST(test_clear_empty_index);
    RUN_TEST(test_clear_index_preserves_metadata);

    /* Combined operations */
    RUN_TEST(test_drop_after_clear);

    /* Open/close index tests */
    RUN_TEST(test_open_index_with_valid_params);
    RUN_TEST(test_open_index_not_found);
    RUN_TEST(test_open_index_null_database);
    RUN_TEST(test_open_index_null_name);
    RUN_TEST(test_open_index_null_output);
    RUN_TEST(test_close_index_null);
    RUN_TEST(test_close_index_frees_resources);
    RUN_TEST(test_open_multiple_indexes);
    RUN_TEST(test_reopen_same_index);
    RUN_TEST(test_open_index_rejects_huge_dimensions);
    RUN_TEST(test_open_index_rejects_huge_block_size);

    /* BLOB I/O tests */
    RUN_TEST(test_blob_spot_create_existing_row);
    RUN_TEST(test_blob_spot_create_nonexistent_row);
    RUN_TEST(test_blob_spot_create_writable);
    RUN_TEST(test_blob_spot_free_null);
    RUN_TEST(test_blob_spot_reload_same_rowid);
    RUN_TEST(test_blob_spot_reload_different_rowid);
    RUN_TEST(test_blob_spot_flush);
    RUN_TEST(test_blob_spot_flush_readonly);
    RUN_TEST(test_blob_spot_create_null_output);

    return UNITY_END();
}
