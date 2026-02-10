# Vtab Phase 2: Metadata Columns

## Summary

Add user-defined metadata columns (`category TEXT`, `score REAL`, etc.) to the virtual table. Stored in `_attrs` shadow table, schema persisted in `_columns` shadow table. Metadata appears in SELECT \* and can be used in Phase 3 for filtered search. 13 new tests.

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

- Parent TPP: `20260210-virtual-table-with-filtering.md` — Tribal Knowledge
- `src/diskann_vtab.c` — After Phase 1 (working basic vtab)
- `src/diskann_api.c` — `diskann_drop_index()` (needs extension for \_attrs/\_columns)
- `src/diskann_util.h` — `validate_identifier()`
- Blocked by: `20260210-vtab-phase1-basic-vtab.md`

## Description

**Problem:** Phase 1 vtab only has vector/distance/k columns. Real-world use needs metadata (categories, timestamps, scores) that can be queried alongside vector search.

**Success criteria:** 13 new tests pass. Can CREATE with metadata columns, INSERT with metadata, SELECT returns metadata, DELETE cleans up metadata, schema survives reopen, DROP cleans up all shadow tables.

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
/* In cursor: sqlite3_stmt *meta_stmt; int64_t meta_cached_rowid; */
if (meta_cached_rowid != current_rowid) {
    sqlite3_reset(meta_stmt);
    sqlite3_bind_int64(meta_stmt, 1, current_rowid);
    sqlite3_step(meta_stmt);
    meta_cached_rowid = current_rowid;
}
/* Read column via sqlite3_column_value(meta_stmt, meta_idx + 1) */
/* +1 because _attrs col 0 is rowid */
```

Prepared statement created in xFilter (need table name), finalized in xClose.

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

### Tests (13)

**CREATE with metadata (5):** 20. `test_vtab_meta_create` — `dimension=3, metric=euclidean, category TEXT, score REAL` succeeds. Verify `_attrs` and `_columns` tables exist. 21. `test_vtab_meta_create_all_types` — `a TEXT, b INTEGER, c REAL, d BLOB` all accepted. 22. `test_vtab_meta_create_invalid_type` — `a DATETIME` rejected. 23. `test_vtab_meta_create_invalid_name` — `123bad TEXT` rejected. 24. `test_vtab_meta_create_reserved_name` — `vector TEXT` rejected, also `distance`, `k`, `rowid`.

**INSERT with metadata (3):** 25. `test_vtab_meta_insert` — INSERT with all metadata. Verify via direct `SELECT` on `_attrs`. 26. `test_vtab_meta_insert_null` — INSERT without metadata columns → NULLs in `_attrs`. 27. `test_vtab_meta_insert_partial` — INSERT with some metadata → remaining are NULL.

**SELECT returns metadata (2):** 28. `test_vtab_meta_search_returns_cols` — Search returns metadata for each result row. 29. `test_vtab_meta_select_star` — `SELECT *` returns metadata (not HIDDEN cols).

**DELETE (1):** 30. `test_vtab_meta_delete` — DELETE removes `_attrs` row too.

**PERSISTENCE (1):** 31. `test_vtab_meta_reopen` — File-based db. Column defs survive close/reopen.

**DROP (1):** 32. `test_vtab_meta_drop` — DROP removes `_attrs` and `_columns` shadow tables.

## Tasks

- [ ] Write 13 failing tests
- [ ] Add test declarations to test_runner.c
- [ ] Implement argument parsing for column defs
- [ ] Create `_columns` and `_attrs` shadow tables in xCreate
- [ ] Implement dynamic declare_vtab
- [ ] Implement xConnect schema reload from `_columns`
- [ ] Implement xColumn metadata fetch with cursor cache
- [ ] Implement xUpdate INSERT metadata insertion
- [ ] Implement xUpdate DELETE metadata cleanup
- [ ] Extend `diskann_drop_index()` for `_attrs`/`_columns`
- [ ] Extend `diskannShadowName` for "attrs"/"columns"
- [ ] All 32 tests pass (19 Phase 1 + 13 Phase 2)
- [ ] `make asan` clean
- [ ] `make clean && make valgrind` clean

## Notes

**Memory management for meta_cols:** Allocated with `sqlite3_malloc` in xCreate/xConnect. Freed in xDisconnect/xDestroy. Each name/type string freed individually, then the array.

**argv[3] and argv[4] are HIDDEN NULLs in INSERT.** Metadata values start at argv[5] (col 3 = argv[2+3] = argv[5]). Must carefully map `meta_col_index` → `argv[2 + 3 + meta_col_index]`.
