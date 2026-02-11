/*
** Virtual table tests for DiskANN
**
** Phase 1: 19 tests covering CREATE/DROP, INSERT, SEARCH, DELETE, PERSISTENCE
** Phase 2: 14 tests covering metadata columns (CREATE/INSERT/SELECT/DELETE/
**          PERSIST/DROP with user-defined metadata columns)
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "unity/unity.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../src/diskann.h"

/* Forward declaration of the entry point (defined in diskann_vtab.c) */
extern int sqlite3_diskann_init(sqlite3 *db, char **pzErrMsg,
                                const sqlite3_api_routines *pApi);

#define VTAB_TEST_DB "/tmp/diskann_test_vtab.db"

/**************************************************************************
** Helpers
**************************************************************************/

static sqlite3 *open_vtab_db(void) {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  rc = sqlite3_diskann_init(db, NULL, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  return db;
}

static void exec_ok(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (err) {
    fprintf(stderr, "SQL error: %s\nSQL: %s\n", err, sql);
    sqlite3_free(err);
  }
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
}

static int exec_expect_error(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (err)
    sqlite3_free(err);
  return rc;
}

static int count_callback(void *pCount, int argc, char **argv, char **cols) {
  (void)argc;
  (void)argv;
  (void)cols;
  (*(int *)pCount)++;
  return 0;
}

static int count_rows(sqlite3 *db, const char *sql) {
  int count = 0;
  sqlite3_exec(db, sql, count_callback, &count, NULL);
  return count;
}

static int table_exists(sqlite3 *db, const char *name) {
  char *sql = sqlite3_mprintf(
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%q'", name);
  int exists = count_rows(db, sql) > 0;
  sqlite3_free(sql);
  return exists;
}

/*
** Helper: create a 3D euclidean vtab named "t" and insert 4 test vectors.
** Returns the db handle. Caller must sqlite3_close(db).
*/
static sqlite3 *create_populated_vtab(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(1, X'0000803f0000000000000000')"); /* [1,0,0] */
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(2, X'000000000000803f00000000')"); /* [0,1,0] */
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(3, X'00000000000000000000803f')"); /* [0,0,1] */
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(4, X'0000803f0000803f00000000')"); /* [1,1,0] */
  return db;
}

/*
** Helper: run a MATCH search with prepared statement, return row count.
** Optionally collect rowids (if out_rowids != NULL) and distances.
*/
static int search_vtab(sqlite3 *db, const char *table, const float *query,
                       int query_bytes, int k, int64_t *out_rowids,
                       float *out_distances, int max_results) {
  char *sql = sqlite3_mprintf("SELECT rowid, distance FROM %s "
                              "WHERE vector MATCH ?1 AND k = ?2",
                              table);
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  sqlite3_bind_blob(stmt, 1, query, query_bytes, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, k);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
    if (out_rowids)
      out_rowids[count] = sqlite3_column_int64(stmt, 0);
    if (out_distances)
      out_distances[count] = (float)sqlite3_column_double(stmt, 1);
    count++;
  }
  sqlite3_finalize(stmt);
  return count;
}

/*
** Helper: run a MATCH search with LIMIT clause.
*/
static int search_vtab_limit(sqlite3 *db, const char *table, const float *query,
                             int query_bytes, int k, int limit,
                             int64_t *out_rowids, int max_results) {
  char *sql = sqlite3_mprintf("SELECT rowid, distance FROM %s "
                              "WHERE vector MATCH ?1 AND k = ?2 LIMIT ?3",
                              table);
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  sqlite3_bind_blob(stmt, 1, query, query_bytes, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, k);
  sqlite3_bind_int(stmt, 3, limit);

  int count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
    if (out_rowids)
      out_rowids[count] = sqlite3_column_int64(stmt, 0);
    count++;
  }
  sqlite3_finalize(stmt);
  return count;
}

/**************************************************************************
** CREATE/DROP tests (5)
**************************************************************************/

void test_vtab_create(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* Shadow tables should exist */
  TEST_ASSERT_TRUE(table_exists(db, "t_shadow"));
  TEST_ASSERT_TRUE(table_exists(db, "t_metadata"));

  sqlite3_close(db);
}

void test_vtab_create_no_dimension(void) {
  sqlite3 *db = open_vtab_db();
  int rc = exec_expect_error(db, "CREATE VIRTUAL TABLE t USING diskann()");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
  sqlite3_close(db);
}

void test_vtab_create_bad_metric(void) {
  sqlite3 *db = open_vtab_db();
  int rc = exec_expect_error(
      db, "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=hamming)");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
  sqlite3_close(db);
}

void test_vtab_drop(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* Verify shadow tables exist before drop */
  TEST_ASSERT_TRUE(table_exists(db, "t_shadow"));
  TEST_ASSERT_TRUE(table_exists(db, "t_metadata"));

  exec_ok(db, "DROP TABLE t");

  /* Shadow tables should be gone */
  TEST_ASSERT_FALSE(table_exists(db, "t_shadow"));
  TEST_ASSERT_FALSE(table_exists(db, "t_metadata"));

  sqlite3_close(db);
}

void test_vtab_create_sql_injection(void) {
  sqlite3 *db = open_vtab_db();

  /* Create a decoy table to verify it's not dropped */
  exec_ok(db, "CREATE TABLE foo (x INTEGER)");

  /* Attempt injection via dimension parameter */
  int rc = exec_expect_error(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(\"dimension=3; DROP TABLE foo\")");

  /* foo should still exist regardless of whether vtab creation succeeded */
  TEST_ASSERT_TRUE(table_exists(db, "foo"));

  /* If creation happened to succeed (sscanf stops at semicolon), that's OK —
   * the key check is that foo was not dropped */
  (void)rc;

  sqlite3_close(db);
}

/**************************************************************************
** INSERT tests (4)
**************************************************************************/

void test_vtab_insert_blob(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* Insert a 3D vector */
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(1, X'0000803f0000000000000000')"); /* [1,0,0] */

  /* Verify via search — should find rowid=1 */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[1];
  int n = search_vtab(db, "t", query, (int)sizeof(query), 1, rowids, NULL, 1);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT64(1, rowids[0]);

  sqlite3_close(db);
}

void test_vtab_insert_no_rowid(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* INSERT without explicit rowid should fail (auto-rowid not supported) */
  int rc = exec_expect_error(
      db, "INSERT INTO t(vector) VALUES (X'0000803f0000000000000000')");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);

  sqlite3_close(db);
}

void test_vtab_insert_wrong_dims(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* Insert a 2D vector (8 bytes) into a 3D table → error */
  int rc = exec_expect_error(
      db, "INSERT INTO t(rowid, vector) VALUES (1, X'0000803f0000803f')");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);

  sqlite3_close(db);
}

void test_vtab_insert_null_vector(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* NULL vector should fail */
  int rc =
      exec_expect_error(db, "INSERT INTO t(rowid, vector) VALUES (1, NULL)");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);

  sqlite3_close(db);
}

/**************************************************************************
** SEARCH tests (7)
**************************************************************************/

void test_vtab_search_basic(void) {
  sqlite3 *db = create_populated_vtab();

  /* Search with k=4, should get all 4 results */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[4];
  int n = search_vtab(db, "t", query, (int)sizeof(query), 4, rowids, NULL, 4);
  TEST_ASSERT_EQUAL_INT(4, n);

  sqlite3_close(db);
}

void test_vtab_search_k(void) {
  sqlite3 *db = create_populated_vtab();

  /* Search with k=2, should get exactly 2 results */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[4];
  int n = search_vtab(db, "t", query, (int)sizeof(query), 2, rowids, NULL, 4);
  TEST_ASSERT_EQUAL_INT(2, n);

  sqlite3_close(db);
}

void test_vtab_search_limit(void) {
  sqlite3 *db = create_populated_vtab();

  /* k=10 LIMIT 2 should get 2 results (LIMIT caps the output) */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[4];
  int n =
      search_vtab_limit(db, "t", query, (int)sizeof(query), 10, 2, rowids, 4);
  TEST_ASSERT_EQUAL_INT(2, n);

  sqlite3_close(db);
}

void test_vtab_search_sorted(void) {
  sqlite3 *db = create_populated_vtab();

  /* Verify distances are ascending (closest first) */
  float query[] = {1.0f, 0.0f, 0.0f};
  float distances[4];
  int n =
      search_vtab(db, "t", query, (int)sizeof(query), 4, NULL, distances, 4);
  TEST_ASSERT_TRUE(n >= 2);

  for (int i = 1; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(distances[i] >= distances[i - 1],
                             "distances not sorted ascending");
  }

  sqlite3_close(db);
}

void test_vtab_search_exact_match(void) {
  sqlite3 *db = create_populated_vtab();

  /* Query = [1,0,0], nearest should be id=1 with distance ≈ 0 */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[4];
  float distances[4];
  int n =
      search_vtab(db, "t", query, (int)sizeof(query), 4, rowids, distances, 4);
  TEST_ASSERT_TRUE(n >= 1);
  TEST_ASSERT_EQUAL_INT64(1, rowids[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, distances[0]);

  sqlite3_close(db);
}

void test_vtab_search_empty(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(
      db,
      "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");

  /* Search on empty table should return 0 results, no crash */
  float query[] = {1.0f, 0.0f, 0.0f};
  int n = search_vtab(db, "t", query, (int)sizeof(query), 4, NULL, NULL, 4);
  TEST_ASSERT_EQUAL_INT(0, n);

  sqlite3_close(db);
}

void test_vtab_search_no_match(void) {
  sqlite3 *db = create_populated_vtab();

  /* SELECT without MATCH clause should return 0 rows */
  int n = count_rows(db, "SELECT rowid FROM t");
  TEST_ASSERT_EQUAL_INT(0, n);

  sqlite3_close(db);
}

/**************************************************************************
** DELETE tests (2)
**************************************************************************/

void test_vtab_delete(void) {
  sqlite3 *db = create_populated_vtab();

  /* Delete rowid=1 */
  exec_ok(db, "DELETE FROM t WHERE rowid = 1");

  /* Search should NOT find rowid=1 */
  float query[] = {1.0f, 0.0f, 0.0f};
  int64_t rowids[4];
  int n = search_vtab(db, "t", query, (int)sizeof(query), 4, rowids, NULL, 4);
  TEST_ASSERT_TRUE(n >= 1);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_NOT_EQUAL(1, (int)rowids[i]);
  }

  sqlite3_close(db);
}

void test_vtab_delete_nonexistent(void) {
  sqlite3 *db = create_populated_vtab();

  /* Delete of non-existent rowid should not crash or error */
  exec_ok(db, "DELETE FROM t WHERE rowid = 999");

  /* All 4 original vectors should still be searchable */
  float query[] = {1.0f, 0.0f, 0.0f};
  int n = search_vtab(db, "t", query, (int)sizeof(query), 4, NULL, NULL, 4);
  TEST_ASSERT_EQUAL_INT(4, n);

  sqlite3_close(db);
}

/**************************************************************************
** PERSISTENCE test (1)
**************************************************************************/

void test_vtab_reopen(void) {
  /* Clean up from any previous failed run */
  unlink(VTAB_TEST_DB);

  /* Phase 1: Create, insert, verify */
  {
    sqlite3 *db;
    int rc = sqlite3_open(VTAB_TEST_DB, &db);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
    rc = sqlite3_diskann_init(db, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

    exec_ok(
        db,
        "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean)");
    exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
                "(1, X'0000803f0000000000000000')"); /* [1,0,0] */
    exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
                "(2, X'000000000000803f00000000')"); /* [0,1,0] */
    exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
                "(3, X'00000000000000000000803f')"); /* [0,0,1] */

    /* Verify search works before close */
    float query[] = {1.0f, 0.0f, 0.0f};
    int64_t rowids[3];
    int n = search_vtab(db, "t", query, (int)sizeof(query), 3, rowids, NULL, 3);
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_EQUAL_INT64(1, rowids[0]);

    sqlite3_close(db);
  }

  /* Phase 2: Reopen (exercises xConnect path), search again */
  {
    sqlite3 *db;
    int rc = sqlite3_open(VTAB_TEST_DB, &db);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
    rc = sqlite3_diskann_init(db, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

    /* Search should find the previously inserted vectors */
    float query[] = {1.0f, 0.0f, 0.0f};
    int64_t rowids[3];
    int n = search_vtab(db, "t", query, (int)sizeof(query), 3, rowids, NULL, 3);
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_EQUAL_INT64(1, rowids[0]);

    sqlite3_close(db);
  }

  /* Clean up */
  unlink(VTAB_TEST_DB);
}

/**************************************************************************
** Phase 2: Metadata column tests (14)
**
** Tests that metadata columns (e.g., category TEXT, score REAL) work
** correctly with CREATE, INSERT, SELECT, DELETE, PERSIST, and DROP.
**************************************************************************/

/*
** Helper: create a 3D euclidean vtab named "t" with metadata columns
** (category TEXT, score REAL) and insert 3 test vectors with metadata.
** Returns the db handle. Caller must sqlite3_close(db).
*/
static sqlite3 *create_meta_vtab(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  /* Insert 3 vectors with metadata using prepared statements */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "INSERT INTO t(rowid, vector, category, score) VALUES (?, ?, ?, ?)",
      -1, &stmt, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  float v1[] = {1.0f, 0.0f, 0.0f}; /* [1,0,0] */
  float v2[] = {0.0f, 1.0f, 0.0f}; /* [0,1,0] */
  float v3[] = {0.0f, 0.0f, 1.0f}; /* [0,0,1] */

  struct {
    int64_t rowid;
    float *vec;
    const char *cat;
    double score;
  } rows[] = {
      {1, v1, "landscape", 0.95},
      {2, v2, "portrait", 0.80},
      {3, v3, "landscape", 0.70},
  };

  for (int i = 0; i < 3; i++) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rows[i].rowid);
    sqlite3_bind_blob(stmt, 2, rows[i].vec, 3 * (int)sizeof(float),
                      SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rows[i].cat, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, rows[i].score);
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, rc);
  }
  sqlite3_finalize(stmt);
  return db;
}

/* ---- CREATE with metadata (6 tests) ---- */

void test_vtab_meta_create(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  /* Shadow tables from Phase 1 should exist */
  TEST_ASSERT_TRUE(table_exists(db, "t_shadow"));
  TEST_ASSERT_TRUE(table_exists(db, "t_metadata"));

  /* Phase 2 shadow tables should exist */
  TEST_ASSERT_TRUE(table_exists(db, "t_attrs"));
  TEST_ASSERT_TRUE(table_exists(db, "t_columns"));

  /* Verify _columns has 2 rows (category, score) */
  TEST_ASSERT_EQUAL_INT(2, count_rows(db, "SELECT * FROM t_columns"));

  sqlite3_close(db);
}

void test_vtab_meta_create_all_types(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, a TEXT, b INTEGER, c REAL, d BLOB)");

  TEST_ASSERT_TRUE(table_exists(db, "t_attrs"));
  TEST_ASSERT_TRUE(table_exists(db, "t_columns"));
  TEST_ASSERT_EQUAL_INT(4, count_rows(db, "SELECT * FROM t_columns"));

  sqlite3_close(db);
}

void test_vtab_meta_create_invalid_type(void) {
  sqlite3 *db = open_vtab_db();
  int rc = exec_expect_error(
      db, "CREATE VIRTUAL TABLE t USING diskann(dimension=3, a DATETIME)");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
  sqlite3_close(db);
}

void test_vtab_meta_create_invalid_name(void) {
  sqlite3 *db = open_vtab_db();
  /* Column name starting with digit should be rejected */
  int rc = exec_expect_error(
      db, "CREATE VIRTUAL TABLE t USING diskann(dimension=3, \"123bad TEXT\")");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
  sqlite3_close(db);
}

void test_vtab_meta_create_reserved_name(void) {
  sqlite3 *db = open_vtab_db();

  /* All reserved names should be rejected */
  const char *reserved[] = {"vector", "distance", "k", "rowid"};
  for (int i = 0; i < 4; i++) {
    char *sql = sqlite3_mprintf(
        "CREATE VIRTUAL TABLE t%d USING diskann(dimension=3, %s TEXT)", i,
        reserved[i]);
    int rc = exec_expect_error(db, sql);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(SQLITE_OK, rc, reserved[i]);
    sqlite3_free(sql);
  }

  sqlite3_close(db);
}

void test_vtab_meta_create_duplicate_col(void) {
  sqlite3 *db = open_vtab_db();
  int rc = exec_expect_error(db, "CREATE VIRTUAL TABLE t USING diskann("
                                 "dimension=3, category TEXT, category TEXT)");
  TEST_ASSERT_NOT_EQUAL(SQLITE_OK, rc);
  sqlite3_close(db);
}

/* ---- INSERT with metadata (3 tests) ---- */

void test_vtab_meta_insert(void) {
  sqlite3 *db = create_meta_vtab();

  /* Verify metadata stored in _attrs via direct query */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "SELECT category, score FROM t_attrs WHERE rowid = 1", -1, &stmt,
      NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));

  const char *cat = (const char *)sqlite3_column_text(stmt, 0);
  TEST_ASSERT_NOT_NULL(cat);
  TEST_ASSERT_EQUAL_STRING("landscape", cat);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.95, sqlite3_column_double(stmt, 1));

  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

void test_vtab_meta_insert_null(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  /* INSERT without metadata columns — they should be NULL */
  exec_ok(db, "INSERT INTO t(rowid, vector) VALUES "
              "(1, X'0000803f0000000000000000')");

  /* Verify _attrs row exists with NULLs */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "SELECT category, score FROM t_attrs WHERE rowid = 1", -1, &stmt,
      NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
  TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(stmt, 0));
  TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(stmt, 1));

  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

void test_vtab_meta_insert_partial(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  /* INSERT with only category specified — score should be NULL */
  sqlite3_stmt *ins;
  int rc = sqlite3_prepare_v2(
      db, "INSERT INTO t(rowid, vector, category) VALUES (?, ?, ?)", -1, &ins,
      NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  float vec[] = {1.0f, 0.0f, 0.0f};
  sqlite3_bind_int64(ins, 1, 1);
  sqlite3_bind_blob(ins, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
  sqlite3_bind_text(ins, 3, "landscape", -1, SQLITE_STATIC);
  TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(ins));
  sqlite3_finalize(ins);

  /* Verify: category = 'landscape', score = NULL */
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db,
                          "SELECT category, score FROM t_attrs WHERE rowid = 1",
                          -1, &stmt, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
  TEST_ASSERT_EQUAL_STRING("landscape",
                           (const char *)sqlite3_column_text(stmt, 0));
  TEST_ASSERT_EQUAL_INT(SQLITE_NULL, sqlite3_column_type(stmt, 1));

  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

/* ---- SELECT returns metadata (2 tests) ---- */

void test_vtab_meta_search_returns_cols(void) {
  sqlite3 *db = create_meta_vtab();

  /* Search for nearest to [1,0,0] — should return rowid=1 with metadata */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db,
                              "SELECT rowid, distance, category, score FROM t "
                              "WHERE vector MATCH ?1 AND k = 3",
                              -1, &stmt, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  float query[] = {1.0f, 0.0f, 0.0f};
  sqlite3_bind_blob(stmt, 1, query, (int)sizeof(query), SQLITE_STATIC);

  /* First result should be rowid=1 (exact match) with correct metadata */
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
  TEST_ASSERT_EQUAL_INT64(1, sqlite3_column_int64(stmt, 0));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.0, sqlite3_column_double(stmt, 1));

  const char *cat = (const char *)sqlite3_column_text(stmt, 2);
  TEST_ASSERT_NOT_NULL(cat);
  TEST_ASSERT_EQUAL_STRING("landscape", cat);
  TEST_ASSERT_FLOAT_WITHIN(0.001, 0.95, sqlite3_column_double(stmt, 3));

  /* Verify all 3 rows have non-NULL category */
  int row_count = 1;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TEST_ASSERT_NOT_EQUAL(SQLITE_NULL, sqlite3_column_type(stmt, 2));
    row_count++;
  }
  TEST_ASSERT_EQUAL_INT(3, row_count);

  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

void test_vtab_meta_select_star(void) {
  sqlite3 *db = create_meta_vtab();

  /* SELECT * should return metadata columns but NOT hidden cols */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "SELECT * FROM t WHERE vector MATCH ?1 AND k = 1", -1, &stmt, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  float query[] = {1.0f, 0.0f, 0.0f};
  sqlite3_bind_blob(stmt, 1, query, (int)sizeof(query), SQLITE_STATIC);
  TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));

  /* SELECT * with schema (vector HIDDEN, distance HIDDEN, k HIDDEN,
  ** category TEXT, score REAL) should return 2 columns: category, score.
  ** HIDDEN columns are excluded from SELECT *. */
  int n_cols = sqlite3_column_count(stmt);
  TEST_ASSERT_EQUAL_INT(2, n_cols);

  /* Verify column names */
  TEST_ASSERT_EQUAL_STRING("category", sqlite3_column_name(stmt, 0));
  TEST_ASSERT_EQUAL_STRING("score", sqlite3_column_name(stmt, 1));

  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

/* ---- DELETE (1 test) ---- */

void test_vtab_meta_delete(void) {
  sqlite3 *db = create_meta_vtab();

  /* Verify _attrs row exists for rowid=1 */
  TEST_ASSERT_EQUAL_INT(
      1, count_rows(db, "SELECT 1 FROM t_attrs WHERE rowid = 1"));

  /* Delete rowid=1 */
  exec_ok(db, "DELETE FROM t WHERE rowid = 1");

  /* _attrs row should be gone */
  TEST_ASSERT_EQUAL_INT(
      0, count_rows(db, "SELECT 1 FROM t_attrs WHERE rowid = 1"));

  /* Other _attrs rows should still exist */
  TEST_ASSERT_EQUAL_INT(2, count_rows(db, "SELECT * FROM t_attrs"));

  sqlite3_close(db);
}

/* ---- PERSISTENCE (1 test) ---- */

void test_vtab_meta_reopen(void) {
  unlink(VTAB_TEST_DB);

  /* Phase 1: Create vtab with metadata, insert data, close */
  {
    sqlite3 *db;
    int rc = sqlite3_open(VTAB_TEST_DB, &db);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
    rc = sqlite3_diskann_init(db, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

    exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
                "dimension=3, metric=euclidean, category TEXT, score REAL)");

    /* Insert with metadata via prepared stmt */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(
        db, "INSERT INTO t(rowid, vector, category, score) VALUES (?, ?, ?, ?)",
        -1, &stmt, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
    float vec[] = {1.0f, 0.0f, 0.0f};
    sqlite3_bind_int64(stmt, 1, 1);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "landscape", -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, 0.95);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);

    sqlite3_close(db);
  }

  /* Phase 2: Reopen (exercises xConnect path) — schema + data preserved */
  {
    sqlite3 *db;
    int rc = sqlite3_open(VTAB_TEST_DB, &db);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);
    rc = sqlite3_diskann_init(db, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

    /* Search should find data with metadata */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
                            "SELECT rowid, category, score FROM t "
                            "WHERE vector MATCH ?1 AND k = 1",
                            -1, &stmt, NULL);
    TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

    float query[] = {1.0f, 0.0f, 0.0f};
    sqlite3_bind_blob(stmt, 1, query, (int)sizeof(query), SQLITE_STATIC);
    TEST_ASSERT_EQUAL_INT(SQLITE_ROW, sqlite3_step(stmt));
    TEST_ASSERT_EQUAL_INT64(1, sqlite3_column_int64(stmt, 0));
    TEST_ASSERT_EQUAL_STRING("landscape",
                             (const char *)sqlite3_column_text(stmt, 1));
    TEST_ASSERT_FLOAT_WITHIN(0.001, 0.95, sqlite3_column_double(stmt, 2));

    sqlite3_finalize(stmt);
    sqlite3_close(db);
  }

  unlink(VTAB_TEST_DB);
}

/* ---- DROP (1 test) ---- */

void test_vtab_meta_drop(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  /* Verify all shadow tables exist */
  TEST_ASSERT_TRUE(table_exists(db, "t_shadow"));
  TEST_ASSERT_TRUE(table_exists(db, "t_metadata"));
  TEST_ASSERT_TRUE(table_exists(db, "t_attrs"));
  TEST_ASSERT_TRUE(table_exists(db, "t_columns"));

  exec_ok(db, "DROP TABLE t");

  /* All shadow tables should be gone */
  TEST_ASSERT_FALSE(table_exists(db, "t_shadow"));
  TEST_ASSERT_FALSE(table_exists(db, "t_metadata"));
  TEST_ASSERT_FALSE(table_exists(db, "t_attrs"));
  TEST_ASSERT_FALSE(table_exists(db, "t_columns"));

  sqlite3_close(db);
}
