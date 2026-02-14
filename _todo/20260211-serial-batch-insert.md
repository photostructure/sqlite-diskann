# Serial Batch Insert Optimization

## Summary

Optimize DiskANN insert throughput for batch workloads using single-connection serial techniques: transaction batching, persistent cache, lazy back-edges, neighbor deduplication, and intra-batch candidates. Target: 4-12x speedup over serial `diskann_insert()` for 500-vector batches. No threading — all target DB drivers are single-connection.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [ ] Cleanup & Documentation — pending Phase 1b, 2, 3
- [ ] Final Review — pending benchmarks + remaining phases

## Required Reading

- `CLAUDE.md` — Project conventions
- `TDD.md` — Testing methodology
- `DESIGN-PRINCIPLES.md` — C coding standards
- `src/diskann_insert.c` — Current insert (lines 177-377)
- `src/diskann_search.c` — Beam search internals
- `src/diskann_cache.c` — BlobCache implementation
- `_research/write-performance-analysis.md` — Full options analysis
- `_todo/20260211-insert-profiling.md` — Phase 0 profiling results (prerequisite)

## Description

**Problem:** Serial `diskann_insert()` is too slow for batch workloads. PhotoStructure imports 100-1000 photos at a time.

**Constraint:** Single SQLite connection only. No parallel WAL readers. All optimization must be serial.

**Success Criteria:** 500-vector batch into 10k+ index at >= 300 inserts/sec (vs current 189/sec) with recall >= 85%.

**Prerequisite complete:** `_todo/20260211-insert-profiling.md` — profiling data collected at 10k scale, decision gates evaluated below.

## Tribal Knowledge

- SAVEPOINT per insert is required because writable BLOBs need transaction context
- BlobCache is created/destroyed per insert (lines 231-236, 369-371) — huge waste for batches
- `insert_shadow_row()` calls `sqlite3_mprintf` + `sqlite3_prepare_v2` every time — should be prepared once
- Phase 2 (back-edges, lines 311-330) flushes each neighbor BLOB individually
- FreshDiskANN paper: collect back-edges in delta structure, apply in single pass
- Reference impl: intra-batch candidates use CPU-only distance computation (no I/O)

### Profiling Results (10k scale, post random_start fix)

| Phase               | Avg (us)  | % of Total | Notes                                 |
| ------------------- | --------- | ---------- | ------------------------------------- |
| search              | 2,970     | 56.1%      | Beam search — inherent cost, dominant |
| phase2 (back-edges) | 1,624     | 30.7%      | Acceptance rate 13% at 10k            |
| phase1 (fwd edges)  | 505       | 9.5%       | Low                                   |
| random_start        | 46        | 0.9%       | **Fixed** (was 26% / 1.5ms)           |
| savepoint           | ~1        | 0.02%      | Negligible                            |
| **Total**           | **5,291** | **100%**   | **189 inserts/sec**                   |

- **Cache hits: 0%** — BlobCache created/destroyed per-insert, ~130 nodes visited per insert
- **Similar vectors 15% faster** — search converges faster (977us vs 2,970us), but phase2 33% more expensive
- Source: `_todo/20260211-insert-profiling.md` Session 2026-02-11

## Solutions

### Phase 1: Transaction Batching + Persistent Cache

Wrap N inserts in a single BEGIN/COMMIT. Persist BlobCache across all inserts in the batch. Cache the prepared INSERT statement.

**API:**

```c
int diskann_begin_batch(DiskAnnIndex *idx);
/* call diskann_insert() N times */
int diskann_end_batch(DiskAnnIndex *idx);
```

**Decision gate:** SAVEPOINT overhead = 0.02% → **SKIP transaction batching**. Cache hit rate = 0% → **DO persistent cache** (top priority).

### Phase 2: Lazy Back-Edges + Batch Repair

Skip Phase 2 (lines 311-330) during insert — store forward edges only. After all inserts, run a repair pass that scans modified neighbors and applies all missing back-edges in one sequential pass with deduplication.

**Decision gate:** Phase 2 = 31% of insert time → just above 30% threshold → **DO this** (second priority). Expected -30% total time but high complexity.

### Phase 3: Intra-Batch Candidates

For similar vectors (same event photos), batch members are likely neighbors of each other. Compute all-pairs distances within batch (CPU-only, no I/O), add close batch members as candidate neighbors. Reduces the number of nodes that must be found via expensive beam search.

**Decision gate:** Search = 56% of insert time → above 50% threshold → **CONSIDER this** (third priority). Only benefits similar-vector batches (PhotoStructure's typical case). Deferred until Phase 1 + 2 measured.

## Tasks

Based on profiling (10k scale, 189 inserts/sec baseline):

- [x] **Phase 1a: Persistent BlobCache across batch** — 0% cache hits on ~130 visited nodes/insert. Expected -10-20% total time. Medium effort.
- [ ] Phase 1b: Prepared statement caching for `insert_shadow_row()` — minor optimization, low effort
- [ ] ~~Phase 1c: Transaction batching~~ — SKIPPED, SAVEPOINT = 0.02% of insert time
- [ ] **Phase 2: Lazy back-edges + batch repair** — 31% of insert time. Expected -30% total time. High effort/complexity. Sub-TPP: `_todo/20260212-lazy-back-edges.md` (validated, Research & Planning complete)
- [ ] Phase 3: Intra-batch candidates — deferred until Phase 1+2 measured
- [ ] Benchmark: 500-vector batch into 10k index, compare serial vs batch
- [ ] Validate recall >= 85% for batch inserts

**Verification:**

```bash
make test       # All existing tests pass + new batch tests
make asan       # No memory errors
make clean && make valgrind  # No leaks
```

## Notes

### 2026-02-11: Profiling Complete, Priorities Established

Phase 0 profiling (`_todo/20260211-insert-profiling.md`) is complete. Key findings:

- **random_start fixed** (commit `ad404e1`) — was 26% of insert time, now 0.9%
- **Search dominates** at 56% — inherent beam search cost, hard to optimize
- **Phase 2 = 31%** — borderline for lazy back-edges, but a real win for batch workloads
- **Cache hits = 0%** — BlobCache per-insert is pure waste; persistent cache is the top optimization
- **SAVEPOINT negligible** — 0.02%, no need for transaction batching

**Recommended implementation order:** Phase 1a (persistent cache) first — medium effort, immediate 10-20% win. Phase 2 (lazy back-edges) second — high effort, 30% win but affects graph quality (needs careful recall testing).

### 2026-02-11: Research & Planning for Phase 1a (Persistent BlobCache)

**Ownership Model — The Core Problem:**

Current flow: `diskann_node_free()` → `blob_spot_free()`. Cache holds borrowed pointers. For persistent cache, cache must own BlobSpots across inserts, but nodes still need valid pointers during Phase 1 + Phase 2.

**Solution: `is_cached` flag on BlobSpot + `owns_blobs` flag on BlobCache**

1. `BlobSpot.is_cached` (default 0): When a cache with `owns_blobs=1` stores a BlobSpot, it sets `is_cached=1`.
2. `diskann_node_free()`: If `blob_spot->is_cached`, skip `blob_spot_free()`. Cache owns it.
3. On cache eviction: Set `evicted->is_cached=0`. Node still references it, `diskann_node_free()` will free it.
4. `blob_cache_deinit()` with `owns_blobs=1`: Iterates LRU chain, `blob_spot_free()` all remaining entries.
5. Per-insert caches (`owns_blobs=0`): No behavioral change. `is_cached` stays 0 throughout.

**Why this works:**

- During a single insert: nodes + cache both reference same BlobSpots. `is_cached=1` prevents node teardown from freeing them.
- On eviction during search (capacity exceeded): `is_cached=0` on evicted entry. Node still has valid pointer, frees it normally during context deinit.
- Between inserts: All nodes freed via `diskann_search_ctx_deinit()`. Cached BlobSpots survive (is_cached=1). Evicted ones freed by node teardown (is_cached=0). No dangling pointers.
- On batch end: `blob_cache_deinit()` frees all remaining BlobSpots.

**Cache data freshness after Phase 2:**

- Phase 2 modifies a node's in-memory buffer (adds back-edge) then flushes to disk.
- Cached copy IS the latest version — in-memory buffer is authoritative.
- `blob_spot_reload()` with same rowid + `is_initialized=1` returns immediately (no stale read).
- This means the persistent cache is BETTER than re-reading from disk.

**SAVEPOINT rollback safety:**

- If `diskann_insert()` fails and SAVEPOINT is rolled back, cached BlobSpots may have stale data (Phase 2 mods rolled back on disk but still in memory).
- Solution: On any insert error during a batch, caller should call `diskann_end_batch()` which clears the cache. The `diskann_end_batch()` function should handle this gracefully.
- `diskann_begin_batch()` could also wrap everything in a single outer transaction (BEGIN/COMMIT) for atomicity.

**New node caching:**

- After flushing the newly inserted node's BlobSpot, put it in the batch cache. Future inserts visiting this node get a cache hit instead of re-reading from disk.

**Files to modify:**

- `src/diskann_blob.h` — Add `is_cached` field to BlobSpot
- `src/diskann_blob.c` — Initialize `is_cached=0` in `blob_spot_create()`
- `src/diskann_cache.h` — Add `owns_blobs` field to BlobCache
- `src/diskann_cache.c` — Eviction frees with `is_cached=0`; deinit frees remaining with `owns_blobs=1`
- `src/diskann_node.c` — `diskann_node_free()` checks `is_cached`
- `src/diskann_internal.h` — Add `BlobCache *batch_cache` to DiskAnnIndex
- `src/diskann.h` — Declare `diskann_begin_batch()` / `diskann_end_batch()`
- `src/diskann_api.c` — Implement begin/end batch
- `src/diskann_insert.c` — Use `idx->batch_cache` when available; cache new node's BlobSpot
- `tests/c/test_cache.c` — New tests for owning cache eviction + deinit
- `tests/c/test_insert.c` — New tests for batch insert (begin/end API, recall, error handling)

### 2026-02-11: Phase 1a Implementation Complete

**What was done:**

1. Added `is_cached` flag to `BlobSpot` (diskann_blob.h)
2. Added `owns_blobs` flag to `BlobCache` (diskann_cache.h)
3. Modified `blob_cache_put()`: sets `is_cached=1` in owning mode
4. Modified `get_free_slot()`: clears `is_cached=0` on eviction in owning mode
5. Modified `blob_cache_deinit()`: frees all BlobSpots via LRU chain in owning mode
6. Modified `diskann_node_free()`: skips `blob_spot_free()` when `is_cached=1`
7. Added `batch_cache` field to `DiskAnnIndex`
8. Implemented `diskann_begin_batch()` / `diskann_end_batch()` in diskann_api.c
9. Modified `diskann_insert()`: uses `idx->batch_cache` when available via `active_cache` pointer
10. New node BlobSpot cached in batch mode after flush (future inserts get cache hit)
11. `diskann_close_index()` cleans up active batch cache (safety net)
12. 12 new tests: 4 cache ownership + 8 batch API
13. Forward declaration for `BlobCache` in diskann_internal.h to avoid circular include

**Verification:**

- `make test`: 204/204 pass (was 192)
- `make asan`: clean (no errors)
- `make clean && make valgrind`: 0 errors, 0 leaks
- Extension builds: `make all` produces diskann.so

**Next: benchmark to measure actual speedup (expected 10-20%)**
