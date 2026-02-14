# Batch API Vtab Integration & Benchmark

## Summary

The C-level batch API (`diskann_begin_batch()`/`diskann_end_batch()`) persists a BlobCache across inserts, but it's never activated through the SQL/vtab path. The benchmark runner and all SQL users only use `BEGIN/COMMIT`, which provides transaction atomicity but not cache persistence. Wire up batch mode in the vtab layer so SQL transactions automatically benefit from the persistent cache, then benchmark the actual speedup.

## Current Phase

- [ ] Research & Planning
- [ ] Test Design
- [ ] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` — Project conventions
- `TDD.md` — Testing methodology
- `DESIGN-PRINCIPLES.md` — C coding standards
- `src/diskann.h` — `diskann_begin_batch()` / `diskann_end_batch()` declarations
- `src/diskann_api.c` — Batch API implementation
- `src/diskann_insert.c` — How `idx->batch_cache` is used when non-NULL
- `src/diskann_vtab.c` — Virtual table xUpdate (INSERT path)
- `src/diskann_cache.h` — BlobCache with `owns_blobs` mode
- `benchmarks/src/runners/diskann-runner.ts` — Current SQL-level `BEGIN/COMMIT` wrapping
- `_todo/20260211-serial-batch-insert.md` — Phase 1a design notes (ownership model, cache freshness)

## Description

**Problem:** Phase 1a (persistent BlobCache) is implemented at the C API level but never reaches production use:

- SQL users wrap inserts in `BEGIN/COMMIT` but `diskann_insert()` still creates/destroys a per-insert cache each time
- Benchmark runner uses SQL `BEGIN/COMMIT` — measured 37% speedup is from per-insert cache, NOT persistent cache
- Cache hits across inserts are 0% because cache doesn't survive between `diskann_insert()` calls

**Constraints:**

- Vtab API doesn't have explicit "begin batch" / "end batch" hooks from SQLite
- Must be transparent to SQL users (no new SQL syntax)
- Must handle errors gracefully (rollback clears cache)
- Must not break existing tests (204/204 passing)

**Success Criteria:**

- SQL inserts inside `BEGIN/COMMIT` automatically use persistent cache
- Benchmark shows measurable speedup over current (432s baseline at 25k)
- All 204 C tests + vtab tests pass
- ASan + Valgrind clean

## Tribal Knowledge

- `diskann_begin_batch()` sets `idx->batch_cache` (owning BlobCache with capacity 100)
- `diskann_insert()` uses `idx->batch_cache` if non-NULL, else creates per-insert cache
- `diskann_end_batch()` frees batch_cache
- `BlobSpot.is_cached=1` prevents double-free when cache owns BlobSpots
- Cache data stays fresh after Phase 2 — in-memory buffer is authoritative
- On SAVEPOINT rollback, cached data may be stale — `diskann_end_batch()` clears cache safely
- vtab xUpdate is called once per INSERT row — no SQLite hook for "transaction started"

## Solutions

### Option A: Lazy batch start in xUpdate (Recommended)

On first INSERT in a vtab, call `diskann_begin_batch()`. Track state with a flag on the vtab cursor or module-level state. Clean up on xDisconnect or transaction end.

SQLite provides `xBegin`/`xCommit`/`xRollback` hooks on virtual tables for exactly this purpose. Use `xBegin` to call `diskann_begin_batch()` and `xCommit`/`xRollback` to call `diskann_end_batch()`.

**Pros:** Fully transparent, works for all SQL users, hooks already exist in SQLite vtab API
**Cons:** Requires implementing xBegin/xCommit/xRollback (currently not implemented)

### Option B: Expose via SQL function

Add `SELECT diskann_begin_batch('index_name')` / `SELECT diskann_end_batch('index_name')` SQL functions.

**Pros:** Explicit control, simple implementation
**Cons:** User must remember to call, error-prone, not transparent

### Option C: TypeScript-only bindings

Expose `beginBatch()`/`endBatch()` in TypeScript layer, call C API via custom SQL.

**Pros:** Works for JS/TS users
**Cons:** Only helps TS users, not general SQL

**Recommendation:** Option A — use vtab transaction hooks. This is the most robust and transparent approach.

## Tasks

- [ ] Research: Confirm SQLite vtab `xBegin`/`xSync`/`xCommit`/`xRollback` API in SQLite docs
- [ ] Read existing `diskann_vtab.c` to understand current vtab module structure
- [ ] Write tests: vtab INSERT inside `BEGIN/COMMIT` verifies cache is active
- [ ] Write tests: vtab INSERT without explicit transaction still works (autocommit)
- [ ] Write tests: ROLLBACK properly clears cache
- [ ] Implement `xBegin` → call `diskann_begin_batch(idx)`
- [ ] Implement `xCommit` → call `diskann_end_batch(idx)`
- [ ] Implement `xRollback` → call `diskann_end_batch(idx)` (same cleanup)
- [ ] Run `make test` — all 204+ tests pass
- [ ] Run `make asan` — no memory errors
- [ ] Run `make clean && make valgrind` — no leaks
- [ ] Run benchmark: `cd benchmarks && npm run bench -- --profile=profiles/medium.json`
- [ ] Compare build time vs 432s baseline
- [ ] Document results in `experiments/experiment-005-batch-vtab.md`

**Verification:**

```bash
make clean && make test   # All tests pass
make asan                 # No memory errors
make clean && make valgrind  # No leaks
cd benchmarks && npm run bench -- --profile=profiles/medium.json  # Measure speedup
```

## Notes

(To be filled during execution)
