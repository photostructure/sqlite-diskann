# Vtab Phase 2: Metadata Columns

## Summary

Add user-defined metadata columns (`category TEXT`, `score REAL`, etc.) to the virtual table. Stored in `_attrs` shadow table, schema persisted in `_columns` shadow table. Metadata appears in SELECT \* and can be used in Phase 3 for filtered search. 13 new tests.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development (14 tests written, all fail for right reasons)
- [x] Implementation (all 159 tests pass, ASan + Valgrind clean)
- [x] Integration (extension builds, smoke test passes with metadata)
- [x] Cleanup & Documentation
- [x] Final Review (159 tests pass, ASan + Valgrind clean, extension smoke test OK)

## Required Reading

- Parent TPP: `20260210-virtual-table-with-filtering.md` — Tribal Knowledge, argv layout
- Phase 1 TPP: `20260210-vtab-phase1-basic-vtab.md` — Gotchas, SAVEPOINT behavior
- `src/diskann_vtab.c` — After Phase 1 (working basic vtab)
- `src/diskann_api.c` — `diskann_drop_index()` (needs extension for \_attrs/\_columns)
- `src/diskann_util.h` — `validate_identifier()`
- Blocked by: `20260210-vtab-phase1-basic-vtab.md` (DONE)

## Description

**Problem:** Phase 1 vtab only has vector/distance/k columns. Real-world use needs metadata (categories, timestamps, scores) that can be queried alongside vector search.

**Success criteria:** 14 new tests pass. Can CREATE with metadata columns, INSERT with metadata, SELECT returns metadata, DELETE cleans up metadata, schema survives reopen, DROP cleans up all shadow tables.

## Tribal Knowledge

- **Metadata SELECT: use named columns, not SELECT \*.** The TPP originally suggested `SELECT * FROM _attrs WHERE rowid=?` with a `+1` offset (because col 0 is rowid). Fragile. Instead: `SELECT "col1", "col2" FROM _attrs WHERE rowid=?` — column indices map directly to meta_cols array.
- **Quote column names with `"%w"` in all dynamic SQL.** `validate_identifier()` accepts SQL keywords like `select`, `from`. These break unquoted. `"%w"` produces `"select"` which is safe.
- **xFilter can be called multiple times per cursor.** Must finalize existing `meta_stmt` before creating a new one. Check and finalize in xFilter before prepare.
- **Error recovery in xCreate.** If `diskann_create_index()` succeeds but `_attrs` CREATE fails, call `diskann_drop_index()` + `diskann_close_index()` to clean up orphaned shadow tables before returning error.
- **INSERT failure atomicity is free.** If `diskann_insert()` succeeds but `_attrs INSERT` fails, returning SQLITE_ERROR from xUpdate lets SQLite's implicit statement transaction roll back ALL changes (shadow + attrs). No manual `diskann_delete()` needed.
- **Backward compat in xConnect.** Phase 1 indexes have no `_columns` table. If `sqlite3_prepare_v2` for `SELECT ... FROM _columns` fails, set `n_meta_cols = 0` — don't return an error.
- **Duplicate column names must be rejected.** SQLite allows duplicate column names in CREATE TABLE, producing a confusing `_attrs` table. Validate in `parse_meta_columns()`.
- **ROWID=0x08 is taken.** Phase 1 used `DISKANN_IDX_ROWID = 0x08`. Phase 3's FILTER bit must use 0x10+.

## Implementation Design

### Argument Parsing

In argv[3+], distinguish config from column definitions:

- Contains `=` → config parameter (`dimension=128`, `metric=cosine`)
- Otherwise → column definition (`category TEXT`, `score REAL`)
- Parse column defs as `name TYPE` with validation:
  - `validate_identifier(name)` must pass
  - TYPE must be one of: TEXT, INTEGER, REAL, BLOB (case-insensitive)
  - Reject reserved names: `vector`, `distance`, `k`, `rowid`

### Data Structures

```c
typedef struct DiskAnnMetaCol {
  char *name;   /* sqlite3_mprintf'd */
  char *type;   /* sqlite3_mprintf'd */
} DiskAnnMetaCol;
```

Add to `diskann_vtab`: `int n_meta_cols; DiskAnnMetaCol *meta_cols;`

### Shadow Tables

**`{name}_columns`** — schema persistence:

```sql
CREATE TABLE "{db}".{name}_columns (name TEXT NOT NULL, type TEXT NOT NULL)
-- One row per metadata column, insertion order preserved
```

**`{name}_attrs`** — per-vector metadata:

```sql
CREATE TABLE "{db}".{name}_attrs (rowid INTEGER PRIMARY KEY, col1 TYPE1, col2 TYPE2, ...)
-- Column names/types from parsed definitions
```

### Dynamic declare_vtab

Build string dynamically using `sqlite3_str`:

```
CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN, category TEXT, score REAL)
```

Metadata columns are NOT hidden → visible in `SELECT *`.

### xCreate Changes

After creating `_shadow` + `_metadata` (via `diskann_create_index()`):

1. Create `_columns` table, insert one row per metadata column
2. Create `_attrs` table with dynamic schema

### xConnect Changes

Read `_columns` table to reconstruct `meta_cols[]` array. Build same dynamic declare_vtab.

### xColumn for Metadata (col >= 3)

Lazy fetch via prepared statement cached in cursor:

```c
/* In cursor: sqlite3_stmt *meta_stmt; int64_t meta_cached_rowid; int meta_has_row; */
if (meta_cached_rowid != current_rowid) {
    sqlite3_reset(meta_stmt);
    sqlite3_bind_int64(meta_stmt, 1, current_rowid);
    meta_has_row = (sqlite3_step(meta_stmt) == SQLITE_ROW);
    meta_cached_rowid = current_rowid;
}
/* Read via sqlite3_result_value(ctx, sqlite3_column_value(meta_stmt, meta_idx)) */
/* No +1 offset — SELECT names columns explicitly, not SELECT * */
```

Prepared statement: `SELECT "col1", "col2" FROM _attrs WHERE rowid = ?`
Created in xFilter (finalize old stmt first!), finalized in xClose.
Use `sqlite3_result_value` + `sqlite3_column_value` for type-agnostic passthrough.

### xUpdate INSERT Changes

After `diskann_insert()` succeeds:

```sql
INSERT INTO {name}_attrs(rowid, col1, col2, ...) VALUES (?, ?, ?, ...)
```

Bind values from argv[5+] (skip argv[2]=vector, argv[3]=distance, argv[4]=k).
Handle NULL metadata columns (user may omit them in INSERT).

### xUpdate DELETE Changes

After `diskann_delete()`:

```sql
DELETE FROM {name}_attrs WHERE rowid = ?
```

### diskann_drop_index() Extension

Add to `diskann_api.c`:

```c
DROP TABLE IF EXISTS "{db}".{name}_attrs
DROP TABLE IF EXISTS "{db}".{name}_columns
```

Uses `IF EXISTS` for backward compatibility with Phase 1 indexes.

### xShadowName Extension

Add "attrs" and "columns" to the shadow name list.

## Test Design

### Tests (14)

**CREATE with metadata (6):** 20. `test_vtab_meta_create` — `dimension=3, metric=euclidean, category TEXT, score REAL` succeeds. Verify `_attrs` and `_columns` tables exist. 21. `test_vtab_meta_create_all_types` — `a TEXT, b INTEGER, c REAL, d BLOB` all accepted. 22. `test_vtab_meta_create_invalid_type` — `a DATETIME` rejected. 23. `test_vtab_meta_create_invalid_name` — `123bad TEXT` rejected. 24. `test_vtab_meta_create_reserved_name` — `vector TEXT` rejected, also `distance`, `k`, `rowid`. 33. `test_vtab_meta_create_duplicate_col` — `category TEXT, category TEXT` rejected.

**INSERT with metadata (3):** 25. `test_vtab_meta_insert` — INSERT with all metadata via prepared stmt. Verify via direct `SELECT` on `_attrs`. 26. `test_vtab_meta_insert_null` — INSERT without metadata columns → NULLs in `_attrs`. 27. `test_vtab_meta_insert_partial` — INSERT specifying only `category` → `score` is NULL in `_attrs`.

**SELECT returns metadata (2):** 28. `test_vtab_meta_search_returns_cols` — MATCH search returns correct metadata per result row. 29. `test_vtab_meta_select_star` — `SELECT *` returns metadata (not HIDDEN cols).

**DELETE (1):** 30. `test_vtab_meta_delete` — DELETE removes `_attrs` row too. Verify via direct SELECT.

**PERSISTENCE (1):** 31. `test_vtab_meta_reopen` — File-based db. Column defs + metadata survive close/reopen.

**DROP (1):** 32. `test_vtab_meta_drop` — DROP removes `_attrs` and `_columns` shadow tables.

## Tasks

- [x] Write 14 failing tests
- [x] Add test declarations to test_runner.c
- [x] Implement argument parsing for column defs
- [x] Create `_columns` and `_attrs` shadow tables in xCreate
- [x] Implement dynamic declare_vtab
- [x] Implement xConnect schema reload from `_columns`
- [x] Implement xColumn metadata fetch with cursor cache
- [x] Implement xUpdate INSERT metadata insertion
- [x] Implement xUpdate DELETE metadata cleanup
- [x] Extend `diskann_drop_index()` for `_attrs`/`_columns`
- [x] Extend `diskannShadowName` for "attrs"/"columns"
- [x] All 33 vtab tests pass (19 Phase 1 + 14 Phase 2)
- [x] `make asan` clean
- [x] `make clean && make valgrind` clean

## Notes

**Memory management for meta_cols:** Allocated with `sqlite3_malloc` in xCreate/xConnect. Freed in xDisconnect/xDestroy. Each name/type string freed individually, then the array.

**argv[3] and argv[4] are HIDDEN NULLs in INSERT.** Metadata values start at argv[5] (col 3 = argv[2+3] = argv[5]). Must carefully map `meta_col_index` → `argv[2 + 3 + meta_col_index]`.
