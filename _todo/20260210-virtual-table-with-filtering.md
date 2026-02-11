# Virtual Table with Filtered ANN Search

## Summary

Implement a SQLite virtual table for DiskANN with metadata columns and filtered search. Filtering happens **during** beam search graph traversal (Filtered-DiskANN algorithm), not before or after. This is a 4-phase build, each with its own TPP.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development (Phase 1 COMPLETE ✅, Phase 2+ remaining)
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md`, `TDD.md`, `DESIGN-PRINCIPLES.md` — Project conventions
- `src/diskann_vtab.c` — Existing broken vtab (will be rewritten)
- `src/diskann_search.c` — Beam search (`search_ctx_mark_visited()` is filter injection point)
- `src/diskann.h` — Public API (8 functions)
- `src/diskann_api.c` — `validate_identifier()` (static, needs extraction)
- `../sqlite-vec/sqlite-vec.c` — Reference vtab implementation (xBestIndex, xShadowName)
- Filtered-DiskANN paper: https://harsha-simhadri.org/pubs/Filtered-DiskANN23.pdf

## Description

**Problem:** The TypeScript API uses `CREATE VIRTUAL TABLE ... USING diskann(...)` but no working virtual table exists. Two broken files compete for the entry point. Beyond basic vtab, real-world use needs metadata filtering integrated into ANN search.

**Desired outcome:**

```sql
CREATE VIRTUAL TABLE photos USING diskann(
  dimension=512, metric=cosine, category TEXT, year INTEGER
);
INSERT INTO photos(rowid, vector, category, year) VALUES (1, X'...', 'landscape', 2024);
SELECT rowid, distance, category FROM photos
  WHERE vector MATCH ? AND category = 'landscape' AND k = 10;
```

**Key constraint:** Filtering must happen DURING graph traversal. Non-matching nodes are still traversed (graph bridges) but excluded from top-K results.

## Related TPPs

Build phases execute sequentially. Each has its own 8-phase lifecycle:

| Phase     | TPP                                       | Tests     | Description                                    |
| --------- | ----------------------------------------- | --------- | ---------------------------------------------- |
| 0 DONE    | `20260210-vtab-phase0-entry-points.md`    | 0 (infra) | Consolidate entry points, extract shared utils |
| 1 DONE    | `20260210-vtab-phase1-basic-vtab.md`      | 19        | CREATE/INSERT/SEARCH/DELETE via SQL            |
| 2         | `20260210-vtab-phase2-metadata.md`        | 13        | Metadata columns, schema persistence           |
| 3         | `20260210-vtab-phase3-filtered-search.md` | 16        | Filter during beam search, C API + SQL         |
| **Total** |                                           | **48**    |                                                |

Phase 4 (Polish — TS bindings, JSON vectors, README) is tracked inline below.

## Tribal Knowledge

- **SQLITE_EXTENSION_INIT conflict:** `SQLITE_EXTENSION_INIT1` redirects ALL `sqlite3_*` calls through a pointer table. Before `INIT2`, those pointers are NULL → segfault. Solution: `diskann_vtab.c` uses `sqlite3.h` directly; thin `diskann_ext.c` has the INIT macros.
- **xUpdate argv layout trap:** With `(vector HIDDEN, distance HIDDEN, k HIDDEN, cat TEXT)`: argv[2]=vector, argv[3]=distance (NULL, skip), argv[4]=k (NULL, skip), argv[5]=cat. HIDDEN columns are present but NULL in INSERT.
- **`diskann_search()` returns count, not DISKANN_OK.** `n < 0` is error; `n >= 0` is result count.
- **xBestIndex is called multiple times.** Must be idempotent. Allocate `idxStr` with `sqlite3_malloc()`, set `needToFreeIdxStr = 1`.
- **HIDDEN columns and SELECT \*.** HIDDEN cols don't appear in `SELECT *` but can be referenced by name. Metadata cols (NOT hidden) appear in `SELECT *`.
- **Filtered-DiskANN paper + Microsoft Rust:** Non-matching nodes MUST still be visited (graph bridges). Filter only gates top-K insertion. `search_ctx_mark_visited()` sets `visited=1` and adds to visited list BEFORE the filter check.
- **Beam width heuristic:** `max(search_list * 2, k * 4)` for filtered search. Tune from recall test results.
- **xBestIndex argv assignment must match xFilter consumption order.** SQLite presents constraints in arbitrary order. Use a two-pass approach: pass 1 records constraint positions, pass 2 assigns argvIndex in the fixed order xFilter expects (MATCH, K, LIMIT, ROWID, then filters). Assigning sequentially as encountered causes argv order mismatches that silently break xFilter.

## Solutions

### Chosen: Filtered-DiskANN with Pre-Computed Rowid Set

1. Metadata stored in `{name}_attrs` shadow table (one row per vector)
2. Column definitions persisted in `{name}_columns` shadow table
3. Filter callback (`DiskAnnFilterFn`) injected into `DiskAnnSearchCtx`
4. xFilter pre-computes matching rowids via SQL on `_attrs`, stores as sorted array
5. During beam search, `search_ctx_mark_visited()` checks filter before top-K promotion
6. Graph traversal unchanged (all edges followed regardless of filter)

**Rejected:** JSON metadata column (slow, no SQL indexing). Extend shadow tables (mixing 4KB BLOB nodes with relational metadata). Bitmap pre-filtering (doesn't fit graph traversal).

## Implementation Design Summary

### Architecture

```
diskann_vtab.c    — All vtab methods, uses sqlite3.h (NO extension macros)
diskann_vtab.h    — Declares diskann_register(sqlite3 *db)
diskann_ext.c     — Thin .so entry point with SQLITE_EXTENSION_INIT1/INIT2
diskann_util.h    — Shared validate_identifier() (static inline)
```

### vtab Schema

```sql
CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN, meta1 TYPE, meta2 TYPE, ...)
```

Col 0=vector, 1=distance, 2=k (all HIDDEN). Col 3+ = metadata (visible in SELECT \*).

### Module Configuration

`iVersion=3` (enables xShadowName). Separate `xCreate` (creates index + shadow tables) and `xConnect` (opens existing index). `xDestroy` drops all shadow tables via `diskann_drop_index()`.

### xBestIndex Encoding

`idxNum` bitmask: MATCH=0x01, K=0x02, LIMIT=0x04, FILTER=0x08. Conditional argvIndex assignment. Phase 3 filter constraints encoded in `idxStr` as comma-separated `"col_offset:op"` pairs.

### Filter Gate (Phase 3)

In `search_ctx_mark_visited()`: ALWAYS mark visited + add to visited list. CONDITIONALLY add to top-K (only if `filter_fn == NULL || filter_fn(rowid, ctx)`). New `DiskAnnFilterFn` typedef + fields in `DiskAnnSearchCtx`. New public API: `diskann_search_filtered()`.

See child TPPs for full implementation details per phase.

## Tasks

### Phase 4: Polish (tracked here — too small for own TPP)

- [ ] Update `src/index.ts` and `src/types.ts` for MATCH operator + metadata columns
- [ ] Support JSON vector input (`'[1.0, 2.0]'` TEXT) in INSERT
- [ ] Improve error messages with `pVtab->base.zErrMsg`
- [ ] Update README with virtual table examples

## Critical Files

**Will modify:** `src/diskann_vtab.c` (rewrite), `src/diskann_search.h` (filter typedef), `src/diskann_search.c` (filter gate), `src/diskann.h` (search_filtered API), `src/diskann_api.c` (drop_index + util extraction), `Makefile`, `tests/c/test_runner.c`

**Will create:** `src/diskann_vtab.h`, `src/diskann_ext.c`, `src/diskann_util.h`, `tests/c/test_vtab.c`

**Will delete:** `src/diskann_extension.c`

## Verification

```bash
make clean && make test          # All tests pass
make asan                        # No memory errors
make clean && make valgrind      # No leaks
sqlite3 :memory: '.load ./build/diskann' \
  "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean, cat TEXT);" \
  "INSERT INTO t(rowid, vector, cat) VALUES (1, X'0000803f0000000000000000', 'a');" \
  "SELECT rowid, distance, cat FROM t WHERE vector MATCH X'0000803f0000000000000000' AND cat='a' AND k=5;"
```

## Notes

**File-based db for reopen tests.** Use `/tmp/diskann_test_vtab.db` and `unlink()` in both setUp and tearDown.

**Index handle lifetime.** vtab keeps `DiskAnnIndex *idx` open for performance. SQLite serializes vtab calls on a single connection, so no concurrency issues between xFilter (read) and xUpdate (write).

**Metadata lazy fetch.** xColumn for metadata runs `SELECT ... FROM _attrs WHERE rowid = ?` with a prepared statement cached per cursor. Create in xFilter, reset+rebind per row, finalize in xClose.
