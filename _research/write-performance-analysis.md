# DiskANN Write Performance Analysis

> Research conducted 2026-02-11. Informed by FreshDiskANN paper (arxiv 2105.09613v1),
> Microsoft DiskANN reference implementation (`../DiskANN`), and codebase profiling.

## Problem

sqlite-diskann inserts are 20-100x slower than sqlite-vec. PhotoStructure users import
100-1000 photos at a time. Current serial insert is ~10ms/vec for build-from-scratch,
but incremental insert cost into large indices has **never been measured**.

## Critical Constraint: Single SQLite Connection

All target DB drivers (better-sqlite3, @photostructure/sqlite, node:sqlite) are
single-threaded and only open a single database connection. This eliminates any
approach that relies on multiple WAL reader connections for parallel beam search.

## Current Insert Path (src/diskann_insert.c:177-377)

```
diskann_insert(idx, id, vector, dims):
  1. Select random start node (SQL query)              — line 199
  2. BEGIN SAVEPOINT (per-insert transaction)           — lines 214-225
  3. Init BlobCache(100) + SearchCtx                    — lines 231-244
  4. diskann_search_internal() (beam search)            — line 246
     → ~100-200 BLOB reads of 40KB each
  5. insert_shadow_row() (SQL INSERT)                   — line 261
  6. Phase 1: Forward edges (new → visited neighbors)   — lines 293-309
  7. Phase 2: Back-edges (visited neighbors → new)      — lines 311-330
     → blob_spot_flush() per neighbor (~50 flushes)
  8. Flush new node's BLOB                              — line 333
  9. RELEASE SAVEPOINT                                  — lines 342-363
```

## Unvalidated Assumptions in Prior Analysis

1. **320ms/insert**: Benchmark data shows 10.7ms/insert average at 100k scale, but
   that's build-from-scratch (early inserts are cheap). Incremental cost is unknown.
2. **60-80% neighbor overlap**: Never measured for similar or random vectors.
3. **Cache hit rate 60%**: Counter exists (line 253-256) but is discarded with `(void)`.
4. **Phase 2 = 200ms**: Back-of-envelope estimate, never profiled.

## Options Evaluated

### Option A: Serial Optimizations Only

Transaction batching + persistent BlobCache across inserts + prepared statement caching.
No threading.

- **Speedup**: 2-5x
- **Effort**: ~200 lines, 2-3 sessions
- **Risk**: Low
- **Viable**: Yes (single connection)

### Option B: Parallel Beam Search + Dedup (Original Intern Proposal)

Multi-threaded beam search via WAL read-only connections + neighbor deduplication.

- **Speedup**: 5-40x (theoretical)
- **Effort**: ~1000 lines, 5-8 sessions
- **Risk**: Medium-High
- **Viable**: **No** — requires multiple SQLite connections

Also missed three optimizations from the reference implementation:

- Intra-batch candidates (batch members as neighbor candidates for each other)
- Bootstrap phase (densify early batch members' edges)
- "Waterfall" problem (batch members can't see each other's edges during search)

### Option C: FreshDiskANN Two-Index Architecture

In-memory temp index for fast inserts + background consolidation merge into disk index.

- **Speedup**: 300-1000x for insert itself; 3-5x end-to-end
- **Effort**: ~2000 lines, 8-12 sessions
- **Risk**: High — dual codebase, crash recovery
- **Viable**: Technically yes, but overkill for batch-then-idle access pattern

### Option D: Lazy Back-Edges + Batch Repair

Skip Phase 2 during insert. Repair all back-edges in a single sequential pass afterward.

- **Speedup**: 2-3x for insert; ~1.5-2x end-to-end with repair
- **Effort**: ~300 lines, 2-3 sessions
- **Risk**: Low-Medium
- **Viable**: Yes (single connection)

### Option E: Graduated Optimization (Chosen)

Phase 0 (measure) → Phase 1 (serial opts) → Phase 2 (lazy back-edges) → Phase 3 (dedup).
Stop when target is met. No threading needed.

- **Speedup**: 4-12x (realistic ceiling without parallel search)
- **Effort**: Incremental, 1-8 sessions depending on how far you go
- **Risk**: Graduated (low → medium)
- **Viable**: Yes

## Reference Implementation Insights (../DiskANN)

The Rust DiskANN `multi_insert()` uses:

1. **Intra-batch candidates**: `around()` window gives nearby batch items as neighbor
   candidates. For temporally correlated data (same photo import), batch members
   ARE likely neighbors. Computed via CPU distance only — no extra I/O.

2. **BackedgeBuffer**: Inline storage for 1-4 edges (zero heap allocation), switches
   to AdjacencyList at 5+. Most back-edges have 1-2 sources in practice.

3. **aggregate_backedges()**: HashMap-based deduplication — neighbor_id → list of
   back-edges from batch. Applied once per unique neighbor.

4. **Bootstrap phase**: When back-edges are sparse, adds extra intra-batch edges
   with `force_saturate=true` to create connectivity.

## FreshDiskANN Paper Key Insight

The paper's StreamingMerge separates insert from back-edge update:

- **Insert Phase**: Search for neighbors, collect back-edge deltas in Δ structure
  (do NOT apply immediately)
- **Patch Phase**: Apply all Δ back-edges in single sequential pass over index

This converts random I/O (seek per neighbor) to sequential I/O (single scan).
Our "lazy back-edges" approach (Option D) captures this insight in simplified form.

## Chosen Approach: Graduated (Option E)

See TPPs:

- `_todo/20260211-insert-profiling.md` — Phase 0: measure actual bottlenecks
- `_todo/20260211-serial-batch-insert.md` — Phases 1-3: serial optimizations
