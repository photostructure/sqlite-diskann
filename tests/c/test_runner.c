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

/* LE serialization tests */
extern void test_le16_roundtrip(void);
extern void test_le32_roundtrip(void);
extern void test_le64_roundtrip(void);
extern void test_le16_zero(void);
extern void test_le64_max(void);

/* Layout calculation tests */
extern void test_max_edges_3d_256block(void);
extern void test_max_edges_768d_too_small(void);
extern void test_max_edges_4d_large_block(void);
extern void test_metadata_offset_3d(void);

/* Node binary format tests */
extern void test_node_bin_init_and_read_vector(void);
extern void test_node_bin_add_and_read_edge(void);
extern void test_node_bin_multiple_edges(void);
extern void test_node_bin_edge_find_idx(void);
extern void test_node_bin_delete_edge(void);
extern void test_node_bin_delete_last_edge(void);
extern void test_node_bin_prune_edges(void);
extern void test_node_bin_replace_existing_edge(void);
extern void test_node_bin_edge_null_outputs(void);

/* Distance calculation tests */
extern void test_distance_l2_orthogonal(void);
extern void test_distance_l2_same(void);
extern void test_distance_l2_known_value(void);
extern void test_distance_cosine_orthogonal(void);
extern void test_distance_cosine_same_direction(void);
extern void test_distance_cosine_opposite(void);
extern void test_distance_dispatch_l2(void);
extern void test_distance_dispatch_cosine(void);

/* Buffer management tests */
extern void test_distance_buffer_insert_idx_empty(void);
extern void test_distance_buffer_insert_idx_beginning(void);
extern void test_distance_buffer_insert_idx_middle(void);
extern void test_distance_buffer_insert_idx_end(void);
extern void test_distance_buffer_insert_idx_full_rejected(void);
extern void test_buffer_insert_basic(void);
extern void test_buffer_insert_evicts_last(void);
extern void test_buffer_delete_basic(void);
extern void test_buffer_delete_first(void);
extern void test_buffer_delete_last(void);

/* Node alloc/free tests */
extern void test_node_alloc_basic(void);
extern void test_node_free_null(void);

/* DiskAnnIndex derived fields tests */
extern void test_open_index_computes_derived_fields(void);

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

    /* LE serialization tests */
    RUN_TEST(test_le16_roundtrip);
    RUN_TEST(test_le32_roundtrip);
    RUN_TEST(test_le64_roundtrip);
    RUN_TEST(test_le16_zero);
    RUN_TEST(test_le64_max);

    /* Layout calculation tests */
    RUN_TEST(test_max_edges_3d_256block);
    RUN_TEST(test_max_edges_768d_too_small);
    RUN_TEST(test_max_edges_4d_large_block);
    RUN_TEST(test_metadata_offset_3d);

    /* Node binary format tests */
    RUN_TEST(test_node_bin_init_and_read_vector);
    RUN_TEST(test_node_bin_add_and_read_edge);
    RUN_TEST(test_node_bin_multiple_edges);
    RUN_TEST(test_node_bin_edge_find_idx);
    RUN_TEST(test_node_bin_delete_edge);
    RUN_TEST(test_node_bin_delete_last_edge);
    RUN_TEST(test_node_bin_prune_edges);
    RUN_TEST(test_node_bin_replace_existing_edge);
    RUN_TEST(test_node_bin_edge_null_outputs);

    /* Distance calculation tests */
    RUN_TEST(test_distance_l2_orthogonal);
    RUN_TEST(test_distance_l2_same);
    RUN_TEST(test_distance_l2_known_value);
    RUN_TEST(test_distance_cosine_orthogonal);
    RUN_TEST(test_distance_cosine_same_direction);
    RUN_TEST(test_distance_cosine_opposite);
    RUN_TEST(test_distance_dispatch_l2);
    RUN_TEST(test_distance_dispatch_cosine);

    /* Buffer management tests */
    RUN_TEST(test_distance_buffer_insert_idx_empty);
    RUN_TEST(test_distance_buffer_insert_idx_beginning);
    RUN_TEST(test_distance_buffer_insert_idx_middle);
    RUN_TEST(test_distance_buffer_insert_idx_end);
    RUN_TEST(test_distance_buffer_insert_idx_full_rejected);
    RUN_TEST(test_buffer_insert_basic);
    RUN_TEST(test_buffer_insert_evicts_last);
    RUN_TEST(test_buffer_delete_basic);
    RUN_TEST(test_buffer_delete_first);
    RUN_TEST(test_buffer_delete_last);

    /* Node alloc/free tests */
    RUN_TEST(test_node_alloc_basic);
    RUN_TEST(test_node_free_null);

    /* DiskAnnIndex derived fields tests */
    RUN_TEST(test_open_index_computes_derived_fields);

    return UNITY_END();
}
