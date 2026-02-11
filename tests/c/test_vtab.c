/*
** Virtual table tests for DiskANN
**
** Phase 1: 19 tests covering CREATE/DROP, INSERT, SEARCH, DELETE, PERSISTENCE
** Phase 2: 14 tests covering metadata columns (CREATE/INSERT/SELECT/DELETE/
**          PERSIST/DROP with user-defined metadata columns)
** Phase 3: 16 tests covering filtered search (5 C API + 11 SQL)
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "unity/unity.h"
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Platform-specific headers for file operations */
#ifdef _WIN32
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

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

/**************************************************************************
** Phase 3: Filtered search tests (16)
**
** Tests 34-38: C API filter tests (diskann_search_filtered)
** Tests 39-49: SQL filter tests (vtab WHERE clauses on metadata columns)
**************************************************************************/

/* --- Filter callback helpers --- */

static int filter_accept_all(int64_t rowid, void *ctx) {
  (void)rowid;
  (void)ctx;
  return 1;
}

static int filter_reject_all(int64_t rowid, void *ctx) {
  (void)rowid;
  (void)ctx;
  return 0;
}

static int filter_accept_odd(int64_t rowid, void *ctx) {
  (void)ctx;
  return (rowid % 2) != 0;
}

/*
** Helper: create a 3D euclidean index with 10 vectors via C API.
** Returns open index in *out_idx. Caller must close with diskann_close_index().
** db handle stored in *out_db. Caller must sqlite3_close().
*/
static void create_filter_index(sqlite3 **out_db, DiskAnnIndex **out_idx) {
  sqlite3 *db;
  int rc = sqlite3_open(":memory:", &db);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  DiskAnnConfig config = {.dimensions = 3,
                          .metric = DISKANN_METRIC_EUCLIDEAN,
                          .max_neighbors = 32,
                          .search_list_size = 100,
                          .insert_list_size = 200,
                          .block_size = 4096};
  rc = diskann_create_index(db, "main", "idx", &config);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", "idx", &idx);
  TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);

  /* Insert 10 vectors spread along x-axis */
  for (int i = 1; i <= 10; i++) {
    float vec[] = {(float)i * 0.1f, 0.0f, 0.0f};
    rc = diskann_insert(idx, (int64_t)i, vec, 3);
    TEST_ASSERT_EQUAL_INT(DISKANN_OK, rc);
  }

  *out_db = db;
  *out_idx = idx;
}

/* ---- C API Filter Tests (5 tests, #34-#38) ---- */

void test_search_filtered_null_filter(void) {
  sqlite3 *db;
  DiskAnnIndex *idx;
  create_filter_index(&db, &idx);

  float query[] = {0.5f, 0.0f, 0.0f};
  DiskAnnResult results[5];

  /* NULL filter should behave identically to diskann_search() */
  int n = diskann_search_filtered(idx, query, 3, 5, results, NULL, NULL);
  TEST_ASSERT_GREATER_THAN(0, n);
  TEST_ASSERT_LESS_OR_EQUAL(5, n);

  /* Compare with unfiltered search */
  DiskAnnResult unfiltered[5];
  int n2 = diskann_search(idx, query, 3, 5, unfiltered);
  TEST_ASSERT_EQUAL_INT(n2, n);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_filtered_accept_all(void) {
  sqlite3 *db;
  DiskAnnIndex *idx;
  create_filter_index(&db, &idx);

  float query[] = {0.5f, 0.0f, 0.0f};
  DiskAnnResult results[5];

  int n = diskann_search_filtered(idx, query, 3, 5, results, filter_accept_all,
                                  NULL);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* Same count as unfiltered */
  DiskAnnResult unfiltered[5];
  int n2 = diskann_search(idx, query, 3, 5, unfiltered);
  TEST_ASSERT_EQUAL_INT(n2, n);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_filtered_reject_all(void) {
  sqlite3 *db;
  DiskAnnIndex *idx;
  create_filter_index(&db, &idx);

  float query[] = {0.5f, 0.0f, 0.0f};
  DiskAnnResult results[5];

  int n = diskann_search_filtered(idx, query, 3, 5, results, filter_reject_all,
                                  NULL);
  TEST_ASSERT_EQUAL_INT(0, n);

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_filtered_odd_only(void) {
  sqlite3 *db;
  DiskAnnIndex *idx;
  create_filter_index(&db, &idx);

  float query[] = {0.5f, 0.0f, 0.0f};
  DiskAnnResult results[5];

  int n = diskann_search_filtered(idx, query, 3, 5, results, filter_accept_odd,
                                  NULL);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* All returned IDs must be odd */
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(results[i].id % 2 != 0,
                             "Expected only odd rowids in results");
  }

  diskann_close_index(idx);
  sqlite3_close(db);
}

void test_search_filtered_validation(void) {
  sqlite3 *db;
  DiskAnnIndex *idx;
  create_filter_index(&db, &idx);

  float query[] = {0.5f, 0.0f, 0.0f};
  DiskAnnResult results[5];

  /* NULL index */
  TEST_ASSERT_LESS_THAN(
      0, diskann_search_filtered(NULL, query, 3, 5, results, NULL, NULL));
  /* NULL query */
  TEST_ASSERT_LESS_THAN(
      0, diskann_search_filtered(idx, NULL, 3, 5, results, NULL, NULL));
  /* NULL results */
  TEST_ASSERT_LESS_THAN(
      0, diskann_search_filtered(idx, query, 3, 5, NULL, NULL, NULL));
  /* Wrong dimensions */
  TEST_ASSERT_LESS_THAN(
      0, diskann_search_filtered(idx, query, 99, 5, results, NULL, NULL));
  /* k = 0 → returns 0 (not an error) */
  TEST_ASSERT_EQUAL_INT(
      0, diskann_search_filtered(idx, query, 3, 0, results, NULL, NULL));

  diskann_close_index(idx);
  sqlite3_close(db);
}

/* ---- SQL Filter Test Helpers ---- */

/*
** Helper: create 3D euclidean vtab "t" with (category TEXT, score REAL)
** and insert 20 vectors.
** IDs 1-10: category='A', score=i*0.1. Vectors spread along x-axis.
** IDs 11-20: category='B', score=i*0.1+1.0. Vectors spread along y-axis.
*/
static sqlite3 *create_filter_vtab(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT, score REAL)");

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "INSERT INTO t(rowid, vector, category, score) VALUES (?, ?, ?, ?)",
      -1, &stmt, NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  for (int i = 1; i <= 20; i++) {
    float vec[3];
    const char *cat;
    double score;
    if (i <= 10) {
      vec[0] = (float)i * 0.1f;
      vec[1] = 0.0f;
      vec[2] = 0.0f;
      cat = "A";
      score = (double)i * 0.1;
    } else {
      vec[0] = 0.0f;
      vec[1] = (float)(i - 10) * 0.1f;
      vec[2] = 0.0f;
      cat = "B";
      score = (double)(i - 10) * 0.1 + 1.0;
    }
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, (int64_t)i);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 4, score);
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, rc);
  }
  sqlite3_finalize(stmt);
  return db;
}

/*
** Helper: run filtered MATCH search. The where_extra is appended after
** the MATCH and k constraints. It should start with " AND ..." if filtering.
** Returns the number of rows.
*/
static int search_vtab_filtered(sqlite3 *db, const char *table,
                                const float *query, int query_bytes, int k,
                                const char *where_extra, int64_t *out_rowids,
                                float *out_distances, int max_results) {
  char *sql = sqlite3_mprintf("SELECT rowid, distance FROM %s "
                              "WHERE vector MATCH ?1 AND k = ?2%s",
                              table, where_extra ? where_extra : "");
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

/* ---- SQL Filter Tests (11 tests, #39-#49) ---- */

void test_vtab_filter_eq(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f}; /* Between A and B clusters */
  int64_t rowids[20];

  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND category = 'A'", rowids, NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* All results must have category 'A' (rowids 1-10) */
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 1 && rowids[i] <= 10,
                             "Expected only category A rowids (1-10)");
  }

  sqlite3_close(db);
}

void test_vtab_filter_eq_other(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND category = 'B'", rowids, NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* All results must have category 'B' (rowids 11-20) */
  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 11 && rowids[i] <= 20,
                             "Expected only category B rowids (11-20)");
  }

  sqlite3_close(db);
}

void test_vtab_filter_gt(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  /* score > 1.0 → only IDs 11-20 (B cluster, scores 1.1-2.0) */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND score > 1.0", rowids, NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 11 && rowids[i] <= 20,
                             "Expected only rowids 11-20 for score > 1.0");
  }

  sqlite3_close(db);
}

void test_vtab_filter_lt(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.2f, 0.0f, 0.0f}; /* Near A cluster */
  int64_t rowids[20];

  /* score < 0.5 → only IDs 1-4 (A cluster, scores 0.1-0.4) */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND score < 0.5", rowids, NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 1 && rowids[i] <= 4,
                             "Expected only rowids 1-4 for score < 0.5");
  }

  sqlite3_close(db);
}

void test_vtab_filter_between(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  /* score >= 0.5 AND score <= 1.5 → IDs 5-15
   * (A: IDs 5-10, scores 0.5-1.0; B: IDs 11-15, scores 1.1-1.5) */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND score >= 0.5 AND score <= 1.5", rowids,
                               NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 5 && rowids[i] <= 15,
                             "Expected only rowids 5-15 for 0.5<=score<=1.5");
  }

  sqlite3_close(db);
}

void test_vtab_filter_multi(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.8f, 0.0f, 0.0f}; /* Near high-end A cluster */
  int64_t rowids[20];

  /* category = 'A' AND score > 0.5 → IDs 6-10 */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND category = 'A' AND score > 0.5", rowids,
                               NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 6 && rowids[i] <= 10,
                             "Expected only rowids 6-10 for A & score>0.5");
  }

  sqlite3_close(db);
}

void test_vtab_filter_no_match(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  /* category = 'C' → no matching rows */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 10,
                               " AND category = 'C'", rowids, NULL, 20);
  TEST_ASSERT_EQUAL_INT(0, n);

  sqlite3_close(db);
}

void test_vtab_filter_all_match(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  /* score > 0.0 → all 20 rows match. With k=10, should be same as unfiltered.
   */
  int n_filtered = search_vtab_filtered(db, "t", query, (int)sizeof(query), 10,
                                        " AND score > 0.0", rowids, NULL, 20);
  int n_unfiltered =
      search_vtab(db, "t", query, (int)sizeof(query), 10, NULL, NULL, 20);
  TEST_ASSERT_EQUAL_INT(n_unfiltered, n_filtered);

  sqlite3_close(db);
}

void test_vtab_filter_ne(void) {
  sqlite3 *db = create_filter_vtab();
  float query[] = {0.5f, 0.5f, 0.0f};
  int64_t rowids[20];

  /* category != 'A' → only B rows (11-20) */
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 20,
                               " AND category != 'A'", rowids, NULL, 20);
  TEST_ASSERT_GREATER_THAN(0, n);

  for (int i = 0; i < n; i++) {
    TEST_ASSERT_TRUE_MESSAGE(rowids[i] >= 11 && rowids[i] <= 20,
                             "Expected only category B rowids (11-20)");
  }

  sqlite3_close(db);
}

void test_vtab_filter_recall(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=128, metric=euclidean, category TEXT)");

  /* Insert 200 vectors (100 per category) with deterministic values */
  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "INSERT INTO t(rowid, vector, category) VALUES (?, ?, ?)", -1, &stmt,
      NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  float vec[128];
  srand(42); /* deterministic seed */
  for (int i = 1; i <= 200; i++) {
    for (int d = 0; d < 128; d++) {
      vec[d] = (float)rand() / (float)RAND_MAX;
    }
    const char *cat = (i <= 100) ? "A" : "B";
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, (int64_t)i);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cat, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, rc);
  }
  sqlite3_finalize(stmt);

  /* Generate a query near category A vectors */
  srand(42); /* Same seed → first generated vector is ID 1 */
  float query[128];
  for (int d = 0; d < 128; d++) {
    query[d] = (float)rand() / (float)RAND_MAX + 0.01f; /* Slightly offset */
  }

  /* Brute-force: find top-10 among category='A' by distance */
  float brute_dists[100];
  int64_t brute_ids[100];
  for (int i = 1; i <= 100; i++) {
    /* Reconstruct vector for ID i (deterministic from seed 42) */
    srand(42);
    for (int skip = 0; skip < (i - 1); skip++) {
      for (int d = 0; d < 128; d++)
        (void)rand();
    }
    float v[128];
    for (int d = 0; d < 128; d++)
      v[d] = (float)rand() / (float)RAND_MAX;

    /* Euclidean distance squared (monotonic, no sqrt needed for ranking) */
    float dist = 0.0f;
    for (int d = 0; d < 128; d++) {
      float diff = query[d] - v[d];
      dist += diff * diff;
    }
    brute_dists[i - 1] = dist;
    brute_ids[i - 1] = (int64_t)i;
  }

  /* Sort brute-force results by distance (insertion sort, small N) */
  for (int i = 1; i < 100; i++) {
    float key_dist = brute_dists[i];
    int64_t key_id = brute_ids[i];
    int j = i - 1;
    while (j >= 0 && brute_dists[j] > key_dist) {
      brute_dists[j + 1] = brute_dists[j];
      brute_ids[j + 1] = brute_ids[j];
      j--;
    }
    brute_dists[j + 1] = key_dist;
    brute_ids[j + 1] = key_id;
  }

  /* Filtered search: category = 'A', k = 10 */
  int64_t ann_ids[10];
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 10,
                               " AND category = 'A'", ann_ids, NULL, 10);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* Compute recall: how many of brute-force top-10 appear in ANN results? */
  int hits = 0;
  for (int i = 0; i < (n < 10 ? n : 10); i++) {
    for (int j = 0; j < 10; j++) {
      if (ann_ids[i] == brute_ids[j]) {
        hits++;
        break;
      }
    }
  }

  float recall = (float)hits / 10.0f;
  /* printf("  Filtered recall@10: %.0f%% (%d/10 hits)\n", recall * 100, hits);
   */
  TEST_ASSERT_TRUE_MESSAGE(recall >= 0.5f,
                           "Filtered recall@10 should be >= 50%");

  sqlite3_close(db);
}

void test_vtab_filter_graph_bridge(void) {
  sqlite3 *db = open_vtab_db();
  exec_ok(db, "CREATE VIRTUAL TABLE t USING diskann("
              "dimension=3, metric=euclidean, category TEXT)");

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(
      db, "INSERT INTO t(rowid, vector, category) VALUES (?, ?, ?)", -1, &stmt,
      NULL);
  TEST_ASSERT_EQUAL_INT(SQLITE_OK, rc);

  /*
  ** Construct a graph where the nearest 'A' node to the query is only
  ** reachable through 'B' bridge nodes:
  **
  ** 1. Insert B cluster near origin (10 nodes): graph connects through these
  ** 2. Insert distant A cluster (5 nodes at x=10..14): far from query
  ** 3. Insert A_near (ID 16) very close to query at [0.05, 0, 0]
  **
  ** The DiskANN graph connects A_near to B cluster members (nearest
  ** neighbors at insert time). When filtering for category='A', the search
  ** must traverse B nodes (graph bridges) to reach A_near.
  */

  /* B cluster near origin: IDs 1-10 */
  for (int i = 1; i <= 10; i++) {
    float vec[] = {(float)i * 0.1f, 0.0f, 0.0f};
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, (int64_t)i);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "B", -1, SQLITE_STATIC);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
  }

  /* Distant A cluster: IDs 11-15 */
  for (int i = 11; i <= 15; i++) {
    float vec[] = {(float)i, 0.0f, 0.0f};
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, (int64_t)i);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "A", -1, SQLITE_STATIC);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
  }

  /* A_near: ID 16, very close to query at origin */
  {
    float vec[] = {0.05f, 0.0f, 0.0f};
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, 16);
    sqlite3_bind_blob(stmt, 2, vec, (int)sizeof(vec), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, "A", -1, SQLITE_STATIC);
    TEST_ASSERT_EQUAL_INT(SQLITE_DONE, sqlite3_step(stmt));
  }

  sqlite3_finalize(stmt);

  /* Query at origin, filter for category='A' */
  float query[] = {0.0f, 0.0f, 0.0f};
  int64_t rowids[6];
  int n = search_vtab_filtered(db, "t", query, (int)sizeof(query), 6,
                               " AND category = 'A'", rowids, NULL, 6);
  TEST_ASSERT_GREATER_THAN(0, n);

  /* A_near (ID 16) should be the closest A node.
  ** If graph bridge works, it appears in results. */
  int found_near = 0;
  for (int i = 0; i < n; i++) {
    if (rowids[i] == 16) {
      found_near = 1;
      break;
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(found_near,
                           "A_near (ID 16) should be found via B graph bridge");

  sqlite3_close(db);
}
