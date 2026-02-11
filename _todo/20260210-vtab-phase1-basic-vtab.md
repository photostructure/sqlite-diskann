# Vtab Phase 1: Basic Virtual Table

## Summary

Rewrite `diskann_vtab.c` with working CREATE/INSERT/SEARCH/DELETE via SQL. Proper xCreate vs xConnect split, MATCH-based search, k parameter, LIMIT support. 19 new tests.

## Current Phase

- [x] Research & Planning (REVISED — deep review completed)
- [x] Test Design (REVISED — corrected API calls, added tribal knowledge)
- [x] Implementation Design (REVISED — fixed xConnect, clarified argv layout)
- [x] Test-First Development (19 tests written + registered, baseline verified)
- [x] Implementation (vtab rewrite + SAVEPOINT fix + xBestIndex argv ordering fix)
- [x] Integration (extension builds, all 145 tests pass)
- [x] Cleanup & Documentation
- [x] Final Review

## Required Reading

- Parent TPP: `20260210-virtual-table-with-filtering.md` — Tribal Knowledge, Notes
- `src/diskann_vtab.c` — Current working vtab (Phase 0 kept it as-is)
- `src/diskann.h` — Public API (8 functions, pay attention to return value semantics)
- `src/diskann_internal.h` — DiskAnnIndex struct (esp. `dimensions` field used in xConnect)
- `src/diskann_api.c` — Full implementations (shadow table names, error codes, SAVEPOINT usage)
- `tests/c/test_integration.c` — Existing test patterns (open_db, gen_vectors, brute_force_knn)
- `tests/c/test_runner.c` — How to register new tests (forward decls + RUN_TEST)
- `_todo/20260210-extension-loading-fix.md` — Why `diskann_sqlite.h` MUST stay (multi-file extension)
- `_todo/20260210-vtab-phase0-entry-points.md` — Phase 0 outcome (no `diskann_register()`)
- `vendor/sqlite/sqlite3.h:7772-7813` — MATCH/LIMIT constraint constants

**Phase 0 is complete.** Phase 0 was revised — it did NOT create `diskann_register()`. The entry point remains `sqlite3_diskann_init(db, pzErrMsg, pApi)`. Tests must call `sqlite3_diskann_init(db, NULL, NULL)` to register the module.

## Description

**Problem:** The current vtab has these deficiencies:

1. **No xCreate/xConnect split** — `diskannConnect` used for both; every reconnect calls `diskann_create_index()` and relies on `DISKANN_ERROR_EXISTS` fallback
2. **EQ search instead of MATCH** — uses `SQLITE_INDEX_CONSTRAINT_EQ` on vector column; MATCH is the standard operator for ANN/FTS-style search
3. **Hardcoded k=10** — no way for users to control result count
4. **No LIMIT support** — `SQLITE_INDEX_CONSTRAINT_LIMIT` not consumed
5. **No distance output** — schema is `(rowid, vector)` with no distance column
6. **No xDestroy** — `DROP TABLE` doesn't drop shadow tables (`xDestroy = xDisconnect`)
7. **No xShadowName** — shadow tables unprotected (iVersion=0)
8. **Schema declares rowid explicitly** — vtabs get rowid implicitly via xRowid

**Success criteria:** 19 new tests pass. Can CREATE, INSERT vectors, SEARCH with `MATCH`+`k`, DELETE, DROP, and reopen a persisted vtab. All 126 existing tests still pass. ASan + Valgrind clean.

## Tribal Knowledge

### C API Signatures (VERIFIED — do not guess)

```c
/* Returns DISKANN_OK, DISKANN_ERROR_EXISTS, or negative error */
int diskann_create_index(sqlite3 *db, const char *db_name,
                         const char *index_name, const DiskAnnConfig *config);

/* Returns DISKANN_OK or negative error. Caller owns *out_index, must close. */
int diskann_open_index(sqlite3 *db, const char *db_name,
                       const char *index_name, DiskAnnIndex **out_index);

/* Safe to call with NULL. Does NOT close db (borrowed reference). */
void diskann_close_index(DiskAnnIndex *idx);

/* Returns DISKANN_OK or negative error. idx->db used internally. */
int diskann_insert(DiskAnnIndex *idx, int64_t id, const float *vector,
                   uint32_t dims);

/* Returns COUNT of results (>= 0), or NEGATIVE error code.
** NOT DISKANN_OK! Check rc < 0 for error, rc >= 0 for success.
** Results are sorted by distance ascending (closest first). */
int diskann_search(DiskAnnIndex *idx, const float *query, uint32_t dims,
                   int k, DiskAnnResult *results);

/* Takes ONLY (idx, id) — NOT (db, db_name, index_name, idx, id).
** Returns DISKANN_OK, DISKANN_ERROR_NOTFOUND, or negative error. */
int diskann_delete(DiskAnnIndex *idx, int64_t id);

/* Returns DISKANN_OK or negative error. Drops _shadow + _metadata tables. */
int diskann_drop_index(sqlite3 *db, const char *db_name,
                       const char *index_name);
```

### DiskAnnIndex Struct Fields (from diskann_internal.h)

```c
struct DiskAnnIndex {
  sqlite3 *db;                  /* borrowed — do NOT close */
  char *db_name;                /* malloc'd, owned */
  char *index_name;             /* malloc'd, owned */
  char *shadow_name;            /* malloc'd, owned (e.g., "idx_shadow") */
  uint32_t dimensions;          /* ← use this in xConnect for declare_vtab */
  uint8_t metric;
  uint32_t max_neighbors;
  uint32_t search_list_size;
  uint32_t insert_list_size;
  uint32_t block_size;
  double pruning_alpha;
  uint32_t nNodeVectorSize;     /* dimensions * sizeof(float) — derived */
  uint32_t nEdgeVectorSize;     /* same as nNodeVectorSize for float32 */
  uint64_t num_reads, num_writes;
};
```

### Shadow Table Names

`diskann_create_index()` creates:

- `{index_name}_shadow` — BLOB node data
- `{index_name}_metadata` — INTEGER key-value pairs

For a vtab named `photos`, shadow tables are `photos_shadow` and `photos_metadata`.

### Module Registration (Phase 0 Outcome)

Phase 0 was revised — `diskann_register()` was NOT created. The entry point is the existing `sqlite3_diskann_init(db, pzErrMsg, pApi)` in `diskann_vtab.c`. In test builds (no `DISKANN_EXTENSION` flag), the `pApi` parameter is unused.

**Tests must call:** `sqlite3_diskann_init(db, NULL, NULL)` to register the module.

### Test File Auto-Discovery

The Makefile uses a wildcard pattern (line 30):

```makefile
TEST_C_SOURCES = $(filter-out %/test_runner.c %/test_stress.c, $(wildcard $(TEST_DIR)/c/test_*.c))
```

Creating `tests/c/test_vtab.c` will auto-include it in builds. You still need to add forward declarations and `RUN_TEST()` lines in `test_runner.c`.

### SQLite Constants (from vendor/sqlite/sqlite3.h)

```c
#define SQLITE_INDEX_CONSTRAINT_MATCH  64   /* line 7804 */
#define SQLITE_INDEX_CONSTRAINT_LIMIT  73   /* line 7813 */
/* SQLITE_INDEX_CONSTRAINT_EQ is 2 */
```

`sqlite3_stricmp()` is available (line 9772) for xShadowName.

### xUpdate argv Layout for HIDDEN Columns

With schema `CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)`:

**INSERT:** `argc = 2 + nColumns = 5`

- `argv[0]` = NULL (signals INSERT)
- `argv[1]` = new rowid (or NULL for auto — we reject NULL)
- `argv[2]` = col 0 = vector (BLOB)
- `argv[3]` = col 1 = distance (NULL — output-only, skip)
- `argv[4]` = col 2 = k (NULL — input-only for search, skip)

**DELETE:** `argc = 1`

- `argv[0]` = rowid to delete

HIDDEN columns ARE present in the argv array — they're just NULL for INSERT. Don't skip them positionally.

### xBestIndex Is Called Multiple Times

SQLite calls xBestIndex multiple times with different constraint combinations to explore query plans. It MUST be idempotent. Don't allocate idxStr unless truly needed. Assign argvIndex values sequentially — only for usable constraints you actually consume.

### ROWID Constraint Handling Required for DELETE

**CRITICAL BUG IN ORIGINAL DESIGN** (found during review):

`DELETE FROM t WHERE rowid = 5` does NOT call xUpdate directly. SQLite first SCANS for the row via xBestIndex/xFilter, then calls xUpdate to delete each found row. Without ROWID constraint handling:

1. xBestIndex sees EQ on `iColumn == -1` (rowid) — no handler → falls back to full scan
2. Full scan (idxNum=0) returns 0 rows
3. xUpdate never called → DELETE silently does nothing

**Fix:** Add `DISKANN_IDX_ROWID = 0x08` to the xBestIndex bitmask. When `iColumn == -1` + `SQLITE_INDEX_CONSTRAINT_EQ`, handle it as a single-row scan. In xFilter, check shadow table existence and return 0 or 1 result rows. Set `SQLITE_INDEX_SCAN_UNIQUE` for the optimizer.

### SAVEPOINT Fails from xUpdate Too — Made Non-Fatal

**WRONG assumption corrected**: The original TPP (and early plan) assumed SAVEPOINTs would work from xUpdate because "the scan is complete". This is **FALSE**. The INSERT/DELETE SQL statement itself is still "in progress" when xUpdate is called. `SAVEPOINT` returns SQLITE_BUSY, just like in xCreate.

**Evidence**: `INSERT INTO t(rowid, vector) VALUES (1, X'...')` failed with "diskann: insert failed (rc=-1)" because `diskann_insert()` line 201 `sqlite3_exec(SAVEPOINT)` returned SQLITE_BUSY.

**Fix applied** (in `diskann_insert.c` and `diskann_api.c`): Made SAVEPOINT creation non-fatal. If it fails (SQLITE_BUSY from vtab context), continue without it. The vtab's implicit transaction provides atomicity for shadow table operations — any failure in xUpdate causes SQLite to roll back the entire statement, including shadow table changes.

- `diskann_insert.c`: `savepoint_active` only set to 1 if SAVEPOINT succeeds; cleanup code is conditional
- `diskann_api.c` (`diskann_delete`): Same pattern — added `savepoint_active` flag, conditional rollback/release

**Existing 126 C API tests still pass** because when called directly (not from vtab), there's no statement in progress, so SAVEPOINT succeeds as before.

## Implementation Design

### Module Struct

```c
static sqlite3_module diskannModule = {
    3,                 /* iVersion — enables xShadowName */
    diskannCreate,     /* xCreate  — creates shadow tables + opens index */
    diskannConnect,    /* xConnect — opens existing index only */
    diskannBestIndex,  /* xBestIndex */
    diskannDisconnect, /* xDisconnect — closes index handle */
    diskannDestroy,    /* xDestroy — drops shadow tables + frees */
    diskannOpen,       /* xOpen */
    diskannClose,      /* xClose */
    diskannFilter,     /* xFilter */
    diskannNext,       /* xNext */
    diskannEof,        /* xEof */
    diskannColumn,     /* xColumn */
    diskannRowid,      /* xRowid */
    diskannUpdate,     /* xUpdate */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    diskannShadowName, /* xShadowName */
    NULL,              /* xIntegrity */
};
```

### vtab + Cursor Structs

```c
typedef struct diskann_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *db_name;       /* sqlite3_mprintf'd copy of argv[1] */
  char *table_name;    /* sqlite3_mprintf'd copy of argv[2] */
  DiskAnnIndex *idx;   /* Opened index (kept open for performance) */
  uint32_t dimensions; /* Cached from idx for dim validation in xUpdate */
} diskann_vtab;

typedef struct diskann_cursor {
  sqlite3_vtab_cursor base;
  DiskAnnResult *results; /* sqlite3_malloc'd result array */
  int num_results;        /* Actual count from diskann_search() */
  int current;            /* Current position (0-based) */
} diskann_cursor;
```

### Schema

`"CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)"`

Column indices: 0=vector, 1=distance, 2=k. ALL HIDDEN — invisible in `SELECT *`, accessible by name.

### xCreate vs xConnect

**Shared helper** `vtab_init(db, db_name, table_name, idx, ppVtab, pzErr)`:

- Call `sqlite3_declare_vtab(db, "CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)")`
- Allocate `diskann_vtab`, populate `db`, `db_name` (mprintf'd), `table_name` (mprintf'd), `idx`, `dimensions`
- Set `*ppVtab`

**xCreate** (`diskannCreate`):

1. Parse argv[3+] for config: `dimension=N`, `metric=X`, `max_degree=N`, `build_search_list_size=N`
2. Validate dimension > 0
3. Call `diskann_create_index(db, argv[1], argv[2], &config)` — treat `DISKANN_ERROR_EXISTS` as OK
4. Call `diskann_open_index(db, argv[1], argv[2], &idx)`
5. Call `vtab_init()`

**xConnect** (`diskannConnect`):

1. Call `diskann_open_index(db, argv[1], argv[2], &idx)` — config comes from persisted metadata
2. **Do NOT parse argv config.** The dimension, metric, etc. are already in the opened `idx` struct.
3. Call `vtab_init()`

If `diskann_open_index()` fails in xConnect, the index doesn't exist → error.

### xDestroy vs xDisconnect

**xDisconnect** (`diskannDisconnect`):

```c
diskann_close_index(p->idx);
sqlite3_free(p->db_name);
sqlite3_free(p->table_name);
sqlite3_free(p);
```

**xDestroy** (`diskannDestroy`):

```c
diskann_close_index(p->idx);
p->idx = NULL;  /* closed, don't use again */
diskann_drop_index(p->db, p->db_name, p->table_name);
sqlite3_free(p->db_name);
sqlite3_free(p->table_name);
sqlite3_free(p);
```

### xBestIndex (TWO-PASS — see gotcha #13)

**CRITICAL:** SQLite presents constraints in arbitrary order. A single-pass approach
that assigns argvIndex sequentially as constraints are encountered will produce
argv in the wrong order for xFilter. Use two passes.

```c
#define DISKANN_IDX_MATCH 0x01
#define DISKANN_IDX_K     0x02
#define DISKANN_IDX_LIMIT 0x04
#define DISKANN_IDX_ROWID 0x08

/* Pass 1: scan constraints, record positions */
int i_match = -1, i_k = -1, i_limit = -1, i_rowid = -1;
int idxNum = 0;

for (int i = 0; i < pInfo->nConstraint; i++) {
  struct sqlite3_index_constraint *c = &pInfo->aConstraint[i];
  if (!c->usable) continue;

  if (c->op == SQLITE_INDEX_CONSTRAINT_MATCH && c->iColumn == 0) {
    i_match = i; idxNum |= DISKANN_IDX_MATCH;
  } else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn == 2) {
    i_k = i; idxNum |= DISKANN_IDX_K;
  } else if (c->op == SQLITE_INDEX_CONSTRAINT_LIMIT) {
    i_limit = i; idxNum |= DISKANN_IDX_LIMIT;
  } else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn == -1) {
    i_rowid = i; idxNum |= DISKANN_IDX_ROWID;
  }
}

/* Pass 2: assign argvIndex in xFilter consumption order */
int next_argv = 1;
if (i_match >= 0) { pInfo->aConstraintUsage[i_match].argvIndex = next_argv++; ... }
if (i_k >= 0)     { pInfo->aConstraintUsage[i_k].argvIndex = next_argv++; ... }
if (i_limit >= 0) { pInfo->aConstraintUsage[i_limit].argvIndex = next_argv++; ... }
if (i_rowid >= 0) { pInfo->aConstraintUsage[i_rowid].argvIndex = next_argv++; ... }

pInfo->idxNum = idxNum;
/* Cost estimation unchanged */
```

### xFilter

```c
int next = 0;
const float *query = NULL;
uint32_t query_dims = 0;
int k = 10;  /* default */

if (idxNum & DISKANN_IDX_MATCH) {
  /* Validate type is BLOB */
  query = (const float *)sqlite3_value_blob(argv[next]);
  int bytes = sqlite3_value_bytes(argv[next]);
  query_dims = (uint32_t)((size_t)bytes / sizeof(float));
  next++;
}
if (idxNum & DISKANN_IDX_K) {
  k = sqlite3_value_int(argv[next]);
  if (k <= 0) k = 10;
  next++;
}
if (idxNum & DISKANN_IDX_LIMIT) {
  int limit = sqlite3_value_int(argv[next]);
  if (limit > 0 && limit < k) k = limit;
  next++;
}

if (!query || query_dims == 0) {
  if (idxNum & DISKANN_IDX_ROWID) {
    /* ROWID scan — check shadow table existence, return 0 or 1 rows */
    sqlite_int64 target = sqlite3_value_int64(argv[next++]);
    char *sql = sqlite3_mprintf(
        "SELECT 1 FROM \"%w\".%s_shadow WHERE id = ?",
        pVtab->db_name, pVtab->table_name);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(pVtab->db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    sqlite3_bind_int64(stmt, 1, target);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      pCur->results = sqlite3_malloc((int)sizeof(DiskAnnResult));
      if (!pCur->results) { sqlite3_finalize(stmt); return SQLITE_NOMEM; }
      pCur->results[0].id = target;
      pCur->results[0].distance = 0.0f;
      pCur->num_results = 1;
    } else {
      pCur->num_results = 0;
    }
    sqlite3_finalize(stmt);
    pCur->current = 0;
    return SQLITE_OK;
  }
  /* No MATCH, no ROWID → empty result set */
  pCur->num_results = 0;
  return SQLITE_OK;
}

/* Allocate and search */
pCur->results = sqlite3_malloc(k * (int)sizeof(DiskAnnResult));
if (!pCur->results) return SQLITE_NOMEM;

int rc = diskann_search(pVtab->idx, query, query_dims, k, pCur->results);
if (rc < 0) {
  sqlite3_free(pCur->results);
  pCur->results = NULL;
  return SQLITE_ERROR;
}
pCur->num_results = rc;  /* rc IS the count, not an error code */
pCur->current = 0;
```

### xColumn

- Col 0 (vector): `sqlite3_result_null(ctx)` — write-only in search context
- Col 1 (distance): `sqlite3_result_double(ctx, (double)results[current].distance)`
- Col 2 (k): `sqlite3_result_null(ctx)` — input-only parameter

### xUpdate

**INSERT** (`argv[0]==NULL, argc == 5`):

```c
if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
  pVtab->base.zErrMsg = sqlite3_mprintf("diskann: rowid required");
  return SQLITE_ERROR;
}
sqlite_int64 rowid = sqlite3_value_int64(argv[1]);

if (sqlite3_value_type(argv[2]) != SQLITE_BLOB) {
  pVtab->base.zErrMsg = sqlite3_mprintf("diskann: vector must be a BLOB");
  return SQLITE_ERROR;
}
const float *vec = (const float *)sqlite3_value_blob(argv[2]);
int bytes = sqlite3_value_bytes(argv[2]);
uint32_t dims = (uint32_t)((size_t)bytes / sizeof(float));

if (dims != p->dimensions) {
  pVtab->base.zErrMsg = sqlite3_mprintf(
    "diskann: dimension mismatch (got %u, expected %u)", dims, p->dimensions);
  return SQLITE_ERROR;
}
/* argv[3]=distance(NULL), argv[4]=k(NULL) — skip */

int rc = diskann_insert(p->idx, rowid, vec, dims);
if (rc == DISKANN_OK) { *pRowid = rowid; return SQLITE_OK; }
pVtab->base.zErrMsg = sqlite3_mprintf("diskann: insert failed (rc=%d)", rc);
return SQLITE_ERROR;
```

**DELETE** (`argc == 1`):

```c
sqlite_int64 rowid = sqlite3_value_int64(argv[0]);
int rc = diskann_delete(p->idx, rowid);
/* NOTFOUND is not an error for DELETE — row may already be gone */
return (rc == DISKANN_OK || rc == DISKANN_ERROR_NOTFOUND) ? SQLITE_OK : SQLITE_ERROR;
```

### xShadowName

```c
static int diskannShadowName(const char *zName) {
  return sqlite3_stricmp(zName, "shadow") == 0 ||
         sqlite3_stricmp(zName, "metadata") == 0;
}
```

### Preserved from Current vtab.c (DO NOT CHANGE)

- `#define DISKANN_VTAB_MAIN` and `#include "diskann_sqlite.h"` — REQUIRED for multi-file extension
- `#ifdef DISKANN_EXTENSION` / `SQLITE_EXTENSION_INIT1` — REQUIRED for extension loading
- `sqlite3_diskann_init()` function at bottom — REQUIRED entry point
- `parse_metric()` helper — reuse as-is

## Test Design

All tests in `tests/c/test_vtab.c`.

### Test Infrastructure

```c
/* Forward declaration of the entry point (defined in diskann_vtab.c) */
extern int sqlite3_diskann_init(sqlite3 *db, char **pzErrMsg,
                                const sqlite3_api_routines *pApi);

#define VTAB_TEST_DB "/tmp/diskann_test_vtab.db"

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
  if (err) sqlite3_free(err);
  return rc;
}

/* Count rows from a query. Callback-based for simplicity. */
static int count_callback(void *pCount, int argc, char **argv, char **cols) {
  (void)argc; (void)argv; (void)cols;
  (*(int *)pCount)++;
  return 0;
}
static int count_rows(sqlite3 *db, const char *sql) {
  int count = 0;
  sqlite3_exec(db, sql, count_callback, &count, NULL);
  return count;
}

/* Check if a table exists in sqlite_master */
static int table_exists(sqlite3 *db, const char *name) {
  char *sql = sqlite3_mprintf(
    "SELECT 1 FROM sqlite_master WHERE type='table' AND name='%q'", name);
  int exists = count_rows(db, sql) > 0;
  sqlite3_free(sql);
  return exists;
}
```

3D float32 BLOB constants:

- `[1,0,0]` = `X'0000803f0000000000000000'`
- `[0,1,0]` = `X'000000000000803f00000000'`
- `[0,0,1]` = `X'00000000000000000000803f'`
- `[1,1,0]` = `X'0000803f0000803f00000000'`

### Tests (19)

**CREATE/DROP (5):**

1. `test_vtab_create` — CREATE VIRTUAL TABLE succeeds; verify `t_shadow` and `t_metadata` exist in `sqlite_master`
2. `test_vtab_create_no_dimension` — `CREATE VIRTUAL TABLE t USING diskann()` → SQLITE_ERROR
3. `test_vtab_create_bad_metric` — `metric=hamming` → SQLITE_ERROR
4. `test_vtab_drop` — `DROP TABLE t` removes shadow tables from `sqlite_master`; verify `t_shadow` and `t_metadata` are gone
5. `test_vtab_create_sql_injection` — `dimension=3; DROP TABLE foo` in args; should either fail or not drop anything

**INSERT (4):**

6. `test_vtab_insert_blob` — `INSERT INTO t(rowid, vector) VALUES (1, X'...')` succeeds; verify via MATCH search returning rowid=1
7. `test_vtab_insert_no_rowid` — `INSERT INTO t(vector) VALUES (X'...')` fails (auto-rowid not supported)
8. `test_vtab_insert_wrong_dims` — Insert 2D vector (8 bytes) into 3D table → error
9. `test_vtab_insert_null_vector` — `INSERT INTO t(rowid, vector) VALUES (1, NULL)` → error

**SEARCH (7):**

10. `test_vtab_search_basic` — Insert 4 vectors ([1,0,0], [0,1,0], [0,0,1], [1,1,0]), search with k=4, get all 4 back
11. `test_vtab_search_k` — Same 4 vectors, k=2 → exactly 2 results
12. `test_vtab_search_limit` — Same 4 vectors, k=10 LIMIT 2 → 2 results
13. `test_vtab_search_sorted` — Verify `distance` column is ascending across returned rows
14. `test_vtab_search_exact_match` — Query=[1,0,0], nearest is id=1, distance ≈ 0.0
15. `test_vtab_search_empty` — Search empty vtab → 0 rows (no crash)
16. `test_vtab_search_no_match` — `SELECT rowid FROM t` (no MATCH clause) → 0 rows

**DELETE (2):**

17. `test_vtab_delete` — Delete rowid=1, subsequent MATCH search doesn't find id=1
18. `test_vtab_delete_nonexistent` — `DELETE FROM t WHERE rowid=999` → no crash, no error

**PERSISTENCE (1):**

19. `test_vtab_reopen` — Open file-based DB (`/tmp/diskann_test_vtab.db`), CREATE, INSERT 3 vectors, close DB, reopen DB (xConnect path), MATCH search finds inserted vectors. Use `unlink()` in setUp AND tearDown.

### Search Test SQL Pattern

```sql
-- Prepared statement approach (recommended for BLOB queries):
SELECT rowid, distance FROM t WHERE vector MATCH ?1 AND k = 4;

-- For tests using sqlite3_exec with hex literals:
-- This is trickier because MATCH with hex literal needs careful SQL.
-- Use sqlite3_prepare_v2 + sqlite3_bind_blob for all search tests.
```

**Important:** Search tests should use `sqlite3_prepare_v2` + `sqlite3_bind_blob` for the query vector, NOT `sqlite3_exec` with hex literals. The MATCH operator on a BLOB column works with bound parameters.

## Tasks

### Test-First Development Phase

- [ ] Create `tests/c/test_vtab.c` with all 19 test functions + helpers
- [ ] Add 19 forward declarations to `tests/c/test_runner.c`
- [ ] Add `/* Virtual table tests */` section with 19 `RUN_TEST()` lines in `test_runner.c:main()`
- [ ] Verify: `make test` compiles — some tests may pass (existing vtab partially works), most search tests should fail
- [ ] Document which tests pass/fail and WHY in the notes below

### Implementation Phase

- [x] Rewrite `diskannCreate` — parse args, call `diskann_create_index()`, call shared init
- [x] Write `diskannConnect` — call `diskann_open_index()` only (no config parsing), call shared init
- [x] Write shared `vtab_init()` helper — declare_vtab, allocate struct, populate fields
- [x] Write `diskannDestroy` — close index, drop shadow tables, free vtab
- [x] Rewrite `diskannBestIndex` — MATCH/K/LIMIT/ROWID bitmask, sequential argvIndex
- [x] Rewrite `diskannFilter` — conditional argv unpacking based on idxNum bits, ROWID scan path
- [x] Rewrite `diskannColumn` — distance output on col 1
- [x] Rewrite `diskannUpdate` — dimension validation, proper argv[2..4] layout for HIDDEN cols
- [x] Implement `diskannShadowName` — return 1 for "shadow" and "metadata"
- [x] Update module struct: iVersion=3, separate xCreate/xConnect/xDestroy, add xShadowName
- [x] Update schema to `"CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)"`
- [x] Make SAVEPOINT non-fatal in `diskann_insert.c` and `diskann_api.c` (vtab context fix)
- [x] Fix xBestIndex argv ordering bug (see Tribal Knowledge below)

### Verification Phase

- [x] All 145 tests pass (`make test`) — 126 existing + 19 new
- [x] `make asan` — ASan clean, no memory errors
- [x] `make clean && make valgrind` — Valgrind clean, 0 errors, 0 leaks
- [x] `make clean && make all` — extension builds (`build/diskann.so`)
- [x] Extension smoke test:
  ```bash
  sqlite3 :memory: ".load ./build/diskann.so" \
    "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean);" \
    "INSERT INTO t(rowid, vector) VALUES (1, X'0000803f0000000000000000');" \
    "SELECT rowid, distance FROM t WHERE vector MATCH X'0000803f0000000000000000' AND k=5;"
  ```
  ✅ Output: `1|0.0` (PASS)

## Notes

### Gotchas for the Implementer

1. **diskann_search() returns COUNT, not DISKANN_OK.** `rc >= 0` is success (count of results), `rc < 0` is error. Do NOT check `rc == DISKANN_OK`.

2. **diskann_delete() takes ONLY (idx, id)** — not the 5-arg form with db/db_name/index_name. The index handle has everything it needs.

3. **diskann_insert() creates its own SAVEPOINT internally.** The vtab xUpdate handler does NOT need to manage transactions for insert.

4. **DISKANN_VTAB_MAIN + diskann_sqlite.h MUST stay** in diskann_vtab.c. This is the multi-file extension solution from Phase 0. Removing it will cause "undefined symbol" errors in the .so build. See `_todo/20260210-extension-loading-fix.md`.

5. **xConnect must NOT parse config from argv.** Config comes from the persisted metadata table via `diskann_open_index()`. The `idx->dimensions` field provides the dimension needed for schema validation.

6. **Persistence test needs file-based DB.** `:memory:` databases cannot be closed and reopened. Use `/tmp/diskann_test_vtab.db` with `unlink()` in both setUp and tearDown to avoid test pollution.

7. **MATCH with BLOB requires prepared statements.** For search tests, use `sqlite3_prepare_v2` + `sqlite3_bind_blob`, not `sqlite3_exec` with string interpolation.

8. **xBestIndex argvIndex is 1-based.** First assigned constraint gets argvIndex=1, second gets 2, etc. Only assign for constraints you actually consume.

9. **DELETE of nonexistent rowid:** `diskann_delete()` returns `DISKANN_ERROR_NOTFOUND`. The vtab should treat this as success (SQLITE_OK) — the row is already gone.

10. **Results array allocation:** Allocate for `k` elements using `sqlite3_malloc()` (not `malloc()`), since vtab code should use SQLite allocator for consistency. Free with `sqlite3_free()` in xClose.

11. **ROWID constraint needed for DELETE to work.** SQLite scans via xBestIndex/xFilter BEFORE calling xUpdate for DELETE. Without ROWID handling (`DISKANN_IDX_ROWID = 0x08`), the scan returns 0 rows and xUpdate is never called. The `test_vtab_delete` test will silently pass-but-wrong without this.

12. **diskann_delete() SAVEPOINT is safe from xUpdate.** Unlike xCreate (where SAVEPOINT failed with SQLITE_BUSY), xUpdate is called after the scan is complete — no SQL statements in progress, so the internal SAVEPOINT works.

13. **xBestIndex argvIndex must be assigned in xFilter consumption order, NOT constraint array order.** SQLite presents constraints in arbitrary order. If K appears before MATCH in the constraint array and argvIndex is assigned sequentially as encountered, argv[0] in xFilter will be the K integer instead of the MATCH blob. The fix is a two-pass approach: pass 1 records constraint positions, pass 2 assigns argvIndex in the fixed order xFilter expects (MATCH, K, LIMIT, ROWID).

## Handoff Notes (2026-02-10)

### Session Summary

Phase 1 is functionally complete. All 19 vtab tests pass, all 126 existing C API tests pass (145 total). ASan and Valgrind clean. Extension builds.

### Bugs Fixed This Session

**xBestIndex argv ordering (the big one):** INSERT via vtab succeeded but MATCH search always returned 0 results. Root cause: SQLite's constraint array order is non-deterministic. When K (op=2, iColumn=2) appeared at constraint[0] and MATCH (op=64, iColumn=0) at constraint[1], the single-pass argvIndex assignment gave K argvIndex=1 and MATCH argvIndex=2. But xFilter always read argv[0] as the MATCH blob — getting the K integer instead, which `sqlite3_value_blob()` returned as NULL, triggering the empty-result early return.

Diagnosed by adding `fprintf(stderr, ...)` to xBestIndex (constraint dump) and xFilter (idxNum + argc). Fix: two-pass xBestIndex — pass 1 records positions, pass 2 assigns argvIndex in MATCH→K→LIMIT→ROWID order.

## Completion Notes (2026-02-10 - Final Session)

### Cleanup & Documentation Phase

**Code Review**: Both `diskann_vtab.c` and `test_vtab.c` reviewed — no cleanup needed. Code is clean, well-commented, follows design principles.

**README Updates**:

- Added Virtual Table Interface section showcasing the new SQL API
- Updated test count from 126 to 145 (126 C API + 19 vtab)
- Reorganized Quick Start to feature vtab interface (recommended) before C API (advanced)
- Added SQL API reference with CREATE/INSERT/SELECT/DELETE/DROP examples

**Extension Smoke Test**: ✅ PASSED

```
sqlite3 :memory: ".load ./build/diskann.so" \
  "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean);" \
  "INSERT INTO t(rowid, vector) VALUES (1, X'0000803f0000000000000000');" \
  "SELECT rowid, distance FROM t WHERE vector MATCH X'0000803f0000000000000000' AND k=5;"
# Output: 1|0.0
```

### Phase 1 Status: COMPLETE ✅

All 8 phases checked off:

1. ✅ Research & Planning
2. ✅ Test Design
3. ✅ Implementation Design
4. ✅ Test-First Development
5. ✅ Implementation
6. ✅ Integration
7. ✅ Cleanup & Documentation
8. ✅ Final Review

All 145 tests pass, ASan/Valgrind clean, extension builds and works via CLI.

**Ready for**: Phase 2 (metadata columns) — another engineer is already working on this.
