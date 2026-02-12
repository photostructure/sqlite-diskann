# Insert Performance Profiling

## Summary

Add timing instrumentation to `diskann_insert()` to measure actual per-phase costs. Every past optimization prediction in this project has been wrong by 2-5x (experiment-001: predicted 5x cache speedup, got 1.6x). We must measure before optimizing.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` — Project conventions
- `TDD.md` — Testing methodology
- `DESIGN-PRINCIPLES.md` — C coding standards
- `src/diskann_insert.c` — Insert path (instrumentation added lines 19-55, 223-231, etc.)
- `src/diskann_search.c` — Beam search
- `src/diskann_cache.c` — BlobCache (has hits/misses counters)
- `_research/write-performance-analysis.md` — Full options analysis

## Description

**Problem:** We don't actually know where insert time is spent. The "320ms per insert" figure is an estimate, not a measurement. Benchmark data shows 10.7ms/insert average at 100k (build-from-scratch), but incremental insert cost into a pre-existing large index has never been profiled.

**Constraints:**

- Instrumentation must not affect production performance (compile-time or env-var gated)
- Must work with existing `make test` / `make asan` / `make valgrind`
- Output should be machine-parseable (for graphing)

**Success Criteria:**

- Per-phase timing breakdown for incremental inserts at 10k, 25k scales
- Neighbor overlap measurement for batches of similar vs random vectors
- Cache hit rate data across sequential inserts
- Data sufficient to decide which optimization phases (from serial-batch-insert TPP) to pursue

## Tribal Knowledge

- BlobCache already has `hits` and `misses` counters — the hit rate was computed but discarded with `(void)hit_rate` (now replaced by visited_count in timing output)
- `clock_gettime(CLOCK_MONOTONIC)` is the right timer for Linux (nanosecond resolution, not affected by wall clock changes)
- Build-from-scratch avg (10.7ms) ≠ incremental insert cost — early inserts into small graph are cheap, late inserts into large graph are expensive
- `insert_list_size` was reduced from 200 to 100 (diskann_api.c) — fewer candidates = fewer BLOB reads
- **`_POSIX_C_SOURCE 199309L` required** before any includes for `clock_gettime` to be declared on Linux with `-pedantic`
- **Timing output only emitted for successful, non-first inserts** — early exits via `goto out` leave timestamps uninitialized, so we guard with `!first && rc == DISKANN_OK`
- **All timespec vars zero-initialized** to avoid garbage if a goto skips a measurement point
- **Cache hits are 0 for all inserts in test suite** — the cache is created per-insert (line 275), never shared across inserts. This confirms that persistent-cache-across-batch is a real optimization opportunity.
- **Phase 2 dominates at scale**: At 200 vectors, Phase 2 = 61% of insert time (5146us of 8428us total), with 130 blob flushes for 199 visited nodes
- **Phase 2 flush count < visited count**: Not every visited node gets a new edge (replace_edge_idx returns -1 when dominated). At id=200: 130 flushes / 199 visited = 65% acceptance rate.
- **Parallel search via multiple WAL reader connections is NOT viable** — all target DB drivers (better-sqlite3, @photostructure/sqlite, node:sqlite) are single-threaded with a single connection
- **Phase 2 dominance was a small-scale artifact** — At 200 vectors: 61%. At 10k: 22%. Phase 2 acceptance rate drops as graph connectivity improves (65% → 13%).
- **random_start is the surprise bottleneck at scale** — 26% of insert time at 10k, growing linearly. `SELECT COUNT(*)` subquery is O(n).
- **Search is the true dominant cost at scale** — 41% at 10k. This is inherent to beam search and harder to optimize.
- **Similar vectors are 15% faster to insert** — search converges faster in a cluster (977us vs 2295us), but phase2 is 33% more expensive (more neighbors accept the back-edge).
- **Profiling CSV output**: `profiling_timing.csv` — 20,998 lines from `make test-profiling`

## Solutions

### Approach: Env-Var Gated Timing + Test Harness

Add `clock_gettime()` calls around each phase in `diskann_insert()`. Gate output behind `DISKANN_DEBUG_TIMING` env var check at function entry (single `getenv()` call, cached in static var). Add a dedicated test that inserts 500 vectors into a pre-existing 10k index and logs timing.

**Status:** Instrumentation implemented and working. Test harness complete (`tests/c/test_profiling.c`).

## Tasks

- [x] Add timing instrumentation to `diskann_insert()` (7 measurement points)
- [x] Verify `make test` passes (192 tests, 0 failures)
- [x] Verify `make asan` passes
- [x] Verify `make valgrind` passes (0 errors, 0 leaks)
- [x] Add test: insert 500 vectors into pre-existing 10k index, log per-phase times
- [x] Add test: insert 500 similar vectors, measure neighbor overlap (visited_list intersection)
- [x] Run at 10k scale, collect data (20,998 timing lines)
- [x] Document findings in Notes section below
- [ ] Run at 25k scale to confirm random_start scaling prediction

**Timing output format (CSV to stderr):**

```
DISKANN_TIMING_HEADER: id,total_us,random_start_us,savepoint_us,search_us,shadow_row_us,phase1_us,phase2_us,flush_new_us,cleanup_us,cache_hits,cache_misses,visited_count,phase2_flushes
DISKANN_TIMING: 200,8428,20,1,1247,22,1911,5146,4,74,0,0,199,130
```

**Verification:**

```bash
DISKANN_DEBUG_TIMING=1 make test 2>timing.log
# 924 timing lines emitted, all clean
make asan   # Passes
make clean && make valgrind  # Passes: 0 errors, 0 leaks
make test-profiling  # Builds 10k index, profiles 500 incremental inserts
# Timing CSV written to profiling_timing.csv
```

## Notes

### Session 2026-02-11: Research + Instrumentation

**What was done:**

1. Comprehensive research into write performance (3 Explore agents + 1 Plan agent):
   - Analyzed current insert path in detail
   - Studied Microsoft DiskANN reference implementation (Rust) for `multi_insert()` patterns
   - Read FreshDiskANN paper (arxiv 2105.09613v1) for streaming insert architecture
   - Reviewed existing benchmarks and experiment results
2. Created research document: `_research/write-performance-analysis.md`
3. Created optimization TPP: `_todo/20260211-serial-batch-insert.md` (blocked by this TPP)
4. Archived intern's TPP: `_done/20260211-batched-inserts.md` (SUPERSEDED)
5. Implemented timing instrumentation in `diskann_insert.c`:
   - 7 measurement points covering all phases
   - Env-var gated (`DISKANN_DEBUG_TIMING=1`)
   - CSV format for machine parsing
   - Header line emitted once on first call
   - Zero-cost when disabled (static cached boolean)

**Preliminary timing data (200-vector integration test, build-from-scratch):**

| Insert # | Total (us) | Random Start | Savepoint | Search | Shadow Row | Phase 1 | Phase 2 | Visited | Flushes |
| -------- | ---------- | ------------ | --------- | ------ | ---------- | ------- | ------- | ------- | ------- |
| 196      | 5500       | 18           | 1         | 1351   | 21         | 1597    | 2451    | 195     | 52      |
| 200      | 8428       | 20           | 1         | 1247   | 22         | 1911    | 5146    | 199     | 130     |

**Key observation (SUPERSEDED by 10k data):** Phase 2 (back-edge updates) is 45-61% of total time at 200 vectors. Search is 15-25%. This was misleading — Phase 2 percentage drops dramatically at scale. See Session 3 below for corrected analysis.

### Session 2026-02-11 (cont.): Test Harness + Valgrind

**What was done:**

1. Verified `make clean && make valgrind` passes (0 errors, 0 leaks, 192 tests)
2. Created `tests/c/test_profiling.c` — separate binary (like test_stress.c):
   - `test_profile_incremental_10k`: Build 10k base index, insert 500 random vectors, verify recall >= 60%
   - `test_profile_similar_vectors`: Build 10k base, insert 500 clustered vectors, verify similar vectors found
3. Updated Makefile:
   - Added `PROFILE_BIN = test_profiling` and `test-profiling` target
   - Added `%/test_profiling.c` to filter-out in `TEST_C_SOURCES`
   - `make test-profiling` automatically sets `DISKANN_DEBUG_TIMING=1` and writes CSV to `profiling_timing.csv`
4. Verified `make test` still passes (192 tests, 0 failures) — profiling tests excluded from main suite
5. Verified profiling binary compiles cleanly with all warnings

**Test-First Development phase complete.** Next phase: Implementation (run the profiling tests and collect data).

### Session 2026-02-11 (cont.): Profiling Results at 10k Scale

**What was done:**

1. Ran `make test-profiling` — both tests pass (2/2, 0 failures)
2. Collected 20,998 timing lines across both tests
3. Analyzed per-phase breakdown at 10k scale

**CRITICAL FINDING: 200-vector data was misleading. Phase priorities reversed at scale.**

#### Random Incremental Inserts (500 inserts into 10k index)

| Phase               | Avg (us)  | % of Total | Priority                   |
| ------------------- | --------- | ---------- | -------------------------- |
| random_start        | 1,463     | 26.2%      | **#2 — easy fix**          |
| search              | 2,295     | 41.0%      | **#1 — biggest cost**      |
| phase1 (fwd edges)  | 492       | 8.8%       | Low                        |
| phase2 (back edges) | 1,261     | 22.6%      | **#3 — still significant** |
| shadow_row          | 35        | 0.6%       | Negligible                 |
| cleanup             | 39        | 0.7%       | Negligible                 |
| **Total**           | **5,594** | **100%**   | **5.6 ms/insert**          |

p50=5,264us p90=7,476us p99=11,145us

#### Similar Vector Inserts (500 clustered inserts into 10k index)

| Phase               | Avg (us)  | % of Total | vs Random                              |
| ------------------- | --------- | ---------- | -------------------------------------- |
| random_start        | 1,533     | 32.1%      | +70us (same neighborhood, same cost)   |
| search              | 977       | 20.4%      | **-57% (converges faster in cluster)** |
| phase1 (fwd edges)  | 507       | 10.6%      | ~same                                  |
| phase2 (back edges) | 1,683     | 35.2%      | **+33% (more neighbors accept edge)**  |
| **Total**           | **4,780** | **100%**   | **-15% faster than random**            |

Phase2 acceptance rate: 22.7% (similar) vs 13.3% (random) — clustered vectors share neighbors, so more back-edges are accepted.

#### Scaling Analysis (base index build, 0-10k)

| ID Range   | Avg Total (us) | Search % | Phase2 %  | RndStart % | Flushes |
| ---------- | -------------- | -------- | --------- | ---------- | ------- |
| 1-1000     | 3,001          | 31.3%    | **49.5%** | 0.8%       | 28.0    |
| 3001-4000  | 3,983          | 44.5%    | 32.9%     | 8.1%       | 21.6    |
| 5001-6000  | 4,331          | 44.7%    | 27.8%     | 14.3%      | 20.6    |
| 7001-8000  | 4,656          | 43.0%    | 25.2%     | 19.5%      | 20.2    |
| 9001-10000 | 4,767          | 42.2%    | **21.2%** | **24.9%**  | 17.3    |

**Key scaling trends:**

- **Phase 2 shrinks with scale**: 49.5% → 21.2% (acceptance rate drops as graph becomes well-connected)
- **random_start grows linearly**: 0.8% → 24.9% (`SELECT COUNT(*)` is O(n) table scan)
- **Search is stable**: ~42-45% after warmup
- **Total cost grows ~60%**: from 3,001us to 4,767us (sublinear)

#### Root Cause: random_start O(n) Table Scan

`diskann_select_random_shadow_row()` uses:

```sql
SELECT rowid FROM shadow LIMIT 1 OFFSET ABS(RANDOM()) % MAX((SELECT COUNT(*) FROM shadow), 1)
```

The `SELECT COUNT(*)` subquery scans the entire shadow table on every call. At 10k rows with 16KB blocks, this scans ~160MB. Cost will be **even worse at 100k**.

#### Cache Hits: Confirmed 0% at Scale

All 20,998 timing lines show `cache_hits=0`. The BlobCache is created and destroyed per-insert (line 283 in diskann_insert.c), never shared. A persistent cache across a batch would eliminate redundant BLOB reads for the ~130 visited nodes.

#### Revised Optimization Priority (based on 10k data)

| Priority | Optimization                                          | Expected Impact                      | Effort |
| -------- | ----------------------------------------------------- | ------------------------------------ | ------ |
| **1**    | **Fix random_start** — cache COUNT or use rowid range | -26% total time (1.5ms → <0.1ms)     | Small  |
| **2**    | **Persistent BlobCache across batch**                 | -10-20% (avoid re-reading 130 nodes) | Medium |
| **3**    | **Lazy back-edges** (defer phase2)                    | -22% total time                      | Large  |
| **4**    | **Transaction batching** (already implemented)        | Already done                         | Done   |

**Previous priority (from 200-vector data) was wrong.** Lazy back-edges was ranked #1 because Phase 2 was 61% at 200 vectors. At 10k it's only 22%. The surprise winner is random_start — trivial to fix, 26% of insert time.

**Next steps:**

1. Fix random_start immediately (cache the count, or use `SELECT rowid FROM shadow WHERE rowid >= ABS(RANDOM()) % max_rowid LIMIT 1`)
2. Update serial-batch-insert TPP with revised priorities
3. Consider running at 25k to see if random_start becomes even worse (predicted: ~40%+ of total)
