# Vtab Phase 1: Basic Virtual Table

## Summary

Rewrite `diskann_vtab.c` with working CREATE/INSERT/SEARCH/DELETE via SQL. Proper xCreate vs xConnect split, MATCH-based search, k parameter, LIMIT support. 19 new tests.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- Parent TPP: `20260210-virtual-table-with-filtering.md` — Tribal Knowledge, Notes
- `src/diskann_vtab.c` — After Phase 0 cleanup
- `src/diskann.h` — Public API (diskann_create_index, open, insert, search, delete, drop)
- `src/diskann_internal.h` — DiskAnnIndex struct
- `tests/c/test_integration.c` — Existing test patterns (open_db, gen_vectors)
- Blocked by: `20260210-vtab-phase0-entry-points.md`

## Description

**Problem:** After Phase 0, `diskann_register()` registers a module with broken methods (xCreate=NULL, EQ instead of MATCH, hardcoded k=10). Need a complete rewrite of all vtab methods.

**Success criteria:** 19 new tests pass. Can CREATE, INSERT vectors, SEARCH with MATCH+k, DELETE, DROP, and reopen a persisted vtab.

## Implementation Design

### Module Struct

```c
static sqlite3_module diskannModule = {
    3,                 /* iVersion — enables xShadowName */
    diskannCreate,     /* xCreate */
    diskannConnect,    /* xConnect */
    diskannBestIndex,  diskannDisconnect, diskannDestroy,
    diskannOpen, diskannClose, diskannFilter, diskannNext, diskannEof,
    diskannColumn, diskannRowid, diskannUpdate,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    diskannShadowName, NULL,
};
```

### vtab + Cursor Structs

```c
typedef struct diskann_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *db_name;       /* argv[1] */
  char *table_name;    /* argv[2] */
  DiskAnnIndex *idx;
  int n_meta_cols;     /* 0 for Phase 1 */
  /* Phase 2 adds: DiskAnnMetaCol *meta_cols */
} diskann_vtab;

typedef struct diskann_cursor {
  sqlite3_vtab_cursor base;
  DiskAnnResult *results;
  int num_results;
  int current;
  /* Phase 2 adds: sqlite3_stmt *meta_stmt, int64_t meta_cached_rowid */
} diskann_cursor;
```

### Schema: `CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)`

Column indices: 0=vector, 1=distance, 2=k. All HIDDEN.

### xCreate vs xConnect

Both parse argv[3+] for config params (`dimension=N`, `metric=X`, etc.) via shared `parse_vtab_args()`. Distinguish `key=value` config from `name TYPE` column defs (Phase 2).

- **xCreate:** Call `diskann_create_index()`, then shared init (open, declare, alloc)
- **xConnect:** Only shared init (open, declare, alloc) — index already exists

If xCreate gets `DISKANN_ERROR_EXISTS`, treat as success (reconnecting to existing table).

### xDestroy

Call `diskann_drop_index(db, db_name, table_name)` then free vtab memory.

### xBestIndex

```
idxNum bitmask: MATCH=0x01, K=0x02, LIMIT=0x04
Constraint detection:
  MATCH: op == SQLITE_INDEX_CONSTRAINT_MATCH && iColumn == 0
  K:     op == SQLITE_INDEX_CONSTRAINT_EQ && iColumn == 2
  LIMIT: op == SQLITE_INDEX_CONSTRAINT_LIMIT
```

Assign argvIndex sequentially for each present constraint. Without MATCH: `estimatedCost = 1e12` (0 rows returned). With MATCH: `estimatedCost = 100`.

### xFilter

Unpack argv based on idxNum bits:

1. If MATCH: extract BLOB query vector. Validate type==SQLITE_BLOB, check dims.
2. If K: `k = sqlite3_value_int(argv[next])`. Default k=10.
3. If LIMIT: `limit = sqlite3_value_int(argv[next])`. Use `min(k, limit)`.
4. Allocate results array, call `diskann_search()`, set `num_results = rc` (rc is count).

### xColumn

- Col 0 (vector): `sqlite3_result_null(ctx)` — not available in search results
- Col 1 (distance): `sqlite3_result_double(ctx, results[current].distance)`
- Col 2 (k): `sqlite3_result_null(ctx)` — input-only

### xUpdate

**INSERT** (`argv[0]==NULL`):

- `rowid = argv[1]` (NULL → error, auto-rowid not supported)
- `vector = argv[2]` (col 0). Validate BLOB, check dims.
- Skip argv[3] (distance, HIDDEN, NULL) and argv[4] (k, HIDDEN, NULL)
- Call `diskann_insert(idx, rowid, vector, dims)`

**DELETE** (`argc==1`):

- `rowid = argv[0]`
- Call `diskann_delete(idx, rowid)`

### xShadowName

Return 1 for "shadow" and "metadata" (case-insensitive via `sqlite3_stricmp`).

## Test Design

All tests in `tests/c/test_vtab.c`. Use `diskann_register(db)` for in-process module registration. Test helpers: `open_vtab_db()`, `exec_ok()`, `exec_expect_error()`.

### Test Infrastructure

```c
static sqlite3 *open_vtab_db(void);    /* open :memory: + register */
static void exec_ok(sqlite3 *db, const char *sql);
static int exec_expect_error(sqlite3 *db, const char *sql);
```

3D float32 BLOB constants:

- `[1,0,0]` = `X'0000803f0000000000000000'`
- `[0,1,0]` = `X'000000000000803f00000000'`
- `[0,0,1]` = `X'00000000000000000000803f'`
- `[1,1,0]` = `X'0000803f0000803f00000000'`

### Tests (19)

**CREATE/DROP (5):**

1. `test_vtab_create` — CREATE succeeds, verify `_shadow` + `_metadata` in sqlite_master
2. `test_vtab_create_no_dimension` — fails with error
3. `test_vtab_create_bad_metric` — `metric=hamming` fails
4. `test_vtab_drop` — DROP removes shadow tables from sqlite_master
5. `test_vtab_create_sql_injection` — SQLite's own argv parsing rejects injection

**INSERT (4):** 6. `test_vtab_insert_blob` — INSERT succeeds, verify via search returning id=1 7. `test_vtab_insert_no_rowid` — INSERT without rowid fails 8. `test_vtab_insert_wrong_dims` — 2D vector into 3D table fails 9. `test_vtab_insert_null_vector` — NULL vector fails

**SEARCH (7):** 10. `test_vtab_search_basic` — 4 vectors, k=4, returns all 4 11. `test_vtab_search_k` — k=2 returns exactly 2 12. `test_vtab_search_limit` — LIMIT 2 returns 2 13. `test_vtab_search_sorted` — distances ascending 14. `test_vtab_search_exact_match` — query=[1,0,0], nearest is id=1, distance≈0 15. `test_vtab_search_empty` — empty table → 0 rows 16. `test_vtab_search_no_match` — SELECT without MATCH → 0 rows

**DELETE (2):** 17. `test_vtab_delete` — DELETE rowid=1, subsequent search doesn't find it 18. `test_vtab_delete_nonexistent` — DELETE rowid=999, no crash

**PERSISTENCE (1):** 19. `test_vtab_reopen` — file-based db, insert, close, reopen, search works

## Tasks

- [ ] Write 19 failing tests in `tests/c/test_vtab.c`
- [ ] Add test declarations to `tests/c/test_runner.c`
- [ ] Verify tests fail for the right reasons
- [ ] Rewrite `diskannCreate` + `diskannConnect` with shared init
- [ ] Rewrite `diskannDestroy` + `diskannDisconnect`
- [ ] Rewrite `diskannBestIndex` with MATCH/K/LIMIT handling
- [ ] Rewrite `diskannFilter` with conditional argv unpacking
- [ ] Rewrite `diskannColumn` for distance output
- [ ] Rewrite `diskannUpdate` for INSERT/DELETE
- [ ] Implement `diskannShadowName`
- [ ] All 19 tests pass
- [ ] `make asan` clean
- [ ] `make clean && make valgrind` clean

## Notes

The `diskannConnect` function currently serves as both xCreate and xConnect. Splitting it requires care: xCreate must call `diskann_create_index()` which fails with `DISKANN_ERROR_EXISTS` if the table already exists. Treat EXISTS as success in xCreate (reconnecting after crash/restart).
