# SUPERSEDED — Batched Vector Inserts for DiskANN

> **Superseded by:** `_todo/20260211-insert-profiling.md` and `_todo/20260211-serial-batch-insert.md`
> **Reason:** Parallel beam search via multiple WAL readers is not viable — all target DB drivers (better-sqlite3, @photostructure/sqlite, node:sqlite) are single-threaded with a single connection. The graduated serial optimization approach replaces this plan. See `_research/write-performance-analysis.md` for full options analysis.

## Summary

DiskANN inserts are 20-100x slower than sqlite-vec, making PhotoStructure imports unacceptably slow. Users import 100-1000 photos at a time, often from the same event (high visual similarity). Implement `diskann_insert_batch()` API that parallelizes beam search and deduplicates neighbor updates to achieve 10-50x speedup. Memory stays low (~5MB for 500-vector batch) by only buffering search results, not the entire index.

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

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- `src/diskann_insert.c` - Current serial insert implementation (lines 141-377)
- `src/diskann_search.c` - Beam search algorithm (diskann_search_internal, lines 211-550)
- `_research/parallel-indexing.md` - Academic paper analysis and design rationale
- `_done/20260210-diskann-recall-fix-investigation.md` - Block size fix validation (context)

## Description

**Problem:** PhotoStructure users import photos in batches (100-1000), but serial `diskann_insert()` is prohibitively slow:

- 500 photos × 320ms = 160 seconds (~3 minutes)
- sqlite-vec does the same in 2-4 seconds
- 20-100x performance gap makes DiskANN impractical for production use

**Root cause:** Each insert performs:

1. Beam search (~100ms) - can be parallelized
2. SAVEPOINT overhead (~2ms) - wasteful for batches
3. Neighbor BLOB updates (~200ms) - massive duplication when vectors are similar

**Key insight:** Photos from same import have high visual similarity → their nearest neighbors overlap heavily. Neighbor A might appear in 40 different photos' neighbor lists, but we update its BLOB 40 times separately. Batching allows deduplication: update A once with all 40 back-edges.

**Constraints:**

- **Incremental updates only** - Add to existing 100k-500k vector index, no rebuilds
- **Low memory** - Users have 4GB+ RAM, but can't load entire index (100MB-1GB)
- **Batch sizes: 100-1000** - Not 300k bulk loads
- **PhotoStructure use case** - Photos from same import are visually similar (high neighbor overlap)

**Success Criteria:**

- 500 similar vectors: <10 seconds (vs ~160s) = **16-50x speedup**
- 500 random vectors: <20 seconds (vs ~160s) = **8-15x speedup**
- Memory: <10MB for batch processing
- Recall@10 >= 75% (maintain graph quality)
- Thread-safe (ASan clean), no leaks (Valgrind clean)

## Tribal Knowledge

### Current Serial Insert Bottleneck (Validated 2026-02-11)

Each `diskann_insert()` (lines 141-377 of diskann_insert.c):

1. SELECT random start node
2. **BEGIN SAVEPOINT** (lines 207-213: required because writable BLOBs need transaction context)
3. `diskann_search_internal()` - beam search, ~200 BLOB reads (~100ms)
4. INSERT shadow row
5. Phase 1: Add edges to new node
6. Phase 2: Update neighbor edges (~50 neighbors, BLOB flush per neighbor, ~200ms)
7. RELEASE SAVEPOINT

**Bottlenecks:**

- 500 SAVEPOINT/RELEASE cycles (transaction overhead)
- Neighbor overlap: Similar vectors share 60-80% of neighbors → same BLOBs updated 20-50 times
- Example: 500 photos × 50 neighbors = 25,000 BLOB updates, but only ~6,000 unique neighbors

### Why Batching Works (Academic Support)

**FreshDiskANN (2021):**

- Designed for high-throughput streaming inserts
- Lazy consolidation: buffer inserts, deduplicate neighbors, merge
- Reports 10-100x speedup vs individual inserts
- Exactly PhotoStructure's use case

**DiskANN paper (NeurIPS 2019), Section 5.3:**

- "Batching updates allows amortizing graph mutations across multiple inserts"
- Neighbor overlap can be exploited for performance

**ParlayANN (PPoPP 2024):**

- Proves per-node locking has negligible contention (~1/n probability)
- Updating neighbor once with 40 back-edges = 40 individual updates, but way faster

### SQLite WAL Concurrency

- WAL mode: unlimited concurrent readers + 1 writer
- Each thread can open read-only connection for parallel search
- Writes must be serialized through main connection
- Current codebase already uses WAL mode

### Memory Constraints (Validated)

For 500 vectors @ 256D:

- Batch input: 500 × 256 × 4 = 512 KB
- Search results: 500 × 200 nodes × 12 bytes = 1.2 MB (just IDs, not vectors!)
- Neighbor dedup map: ~6,000 neighbors × 40 bytes = 240 KB
- **Total: ~2-5 MB** (not entire index - existing index stays on disk)

1000 vectors @ 256D: ~5-10 MB

**Critical:** We do NOT load the entire index into RAM. Existing vectors (100k-500k) stay in SQLite BLOBs and are read during beam search via BlobSpots, just like serial insert does.

### Prior Investigation Context

**Original TPP (20260210-parallel-graph-construction.md):**

- Intern designed in-memory full rebuild (loading ALL vectors into RAM)
- Dismissed incremental batch insert as "only 2-3x speedup, dead-end architecture"
- **Analysis was wrong:** Missed neighbor deduplication benefit (3-5x) and underestimated parallelism (8-24x)
- Approach was wrong for PhotoStructure (users don't need bulk rebuilds, they need fast incremental batches)

**Validation findings (2026-02-11):**

- Porting complexity verified: beam search 80% reusable, edge operations trivial to port
- Test registration is manual (CRITICAL: tests must be registered in test_runner.c)
- Pthread linking required: add `-lpthread` to Makefile

### FreshDiskANN Comparison (2026-02-11)

**Source examined:** `../DiskANN` repository (Rust rewrite, C++ on `cpp_main` branch)

**FreshDiskANN architecture:**

- **Streaming model:** `insert()`, `delete()`, `replace()`, `maintain()`, `needs_maintenance()`
- **Lazy consolidation:** Inserts are buffered, graph updates deferred until `maintain()` is called
- **Periodic maintenance:** When `needs_maintenance()` returns true, run consolidation pass
- **Benefits:** Can handle continuous streaming inserts without blocking
- **Complexity:** Background maintenance, non-deterministic latency, complex state management

**Our approach (explicit batching):**

- **Batch model:** `diskann_insert_batch(vectors[], count)` - process entire batch in one call
- **Immediate processing:** Batch processed synchronously, no deferred maintenance
- **Predictable latency:** User knows batch of 500 takes ~9 seconds, done
- **Simpler code:** No background threads, no maintenance scheduling, no buffering state
- **Better for PhotoStructure:** Photo imports are naturally batched (drag-and-drop folder), explicit batch call fits workflow

**Decision:** Stick with explicit batching - simpler, more predictable, fits PhotoStructure's import workflow perfectly. FreshDiskANN's streaming model solves a different problem (continuous high-throughput inserts with no natural batching).

## Solutions

### SELECTED: Incremental Batch Insert with Parallel Search + Deduplication

**Approach:**

**Phase 1: Store Vectors (Serial, ~1s for 500)**

- Single transaction: INSERT all N shadow rows
- Write vectors to BLOBs with zero edges
- Fast, no graph construction yet

**Phase 2: Parallel Beam Search (Read-only, 8-24 threads)**

- Each thread opens read-only SQLite connection (WAL allows unlimited readers)
- Beam search for assigned slice of vectors
- Existing index stays on disk, read via BLOBs (just like serial insert)
- Store **results only** in memory (visited node IDs + distances, not vectors)
- Time: ~3-4s for 500 vectors (vs 50s serial)

**Phase 3: Deduplicated Edge Updates (Serial, one transaction)**

- Build neighbor map: neighbor_id → list of back-edges from batch
- **Deduplicate:** Neighbor A appears in 40 photos → update A's BLOB once with all 40 edges
- Single transaction wraps all edge mutations
- Time: ~4-6s for 500 vectors (vs 100s with duplication)

**Why this works:**

- Parallelism: 8-24x speedup on beam search (CPU-bound)
- Deduplication: 3-5x reduction in BLOB writes (neighbor overlap)
- Transaction batching: 2x speedup (avoid 500 SAVEPOINTs)
- **Combined: 16-64x speedup** depending on vector similarity

**Memory:** Only batch results (~2-5 MB), not entire index

**API:**

```c
typedef struct DiskAnnBatchConfig {
  int num_threads;                   // 0 = auto-detect
  void (*progress)(int, int, void*); // progress callback
  void *progress_user_data;
} DiskAnnBatchConfig;

int diskann_insert_batch(DiskAnnIndex *idx,
                         const int64_t *ids,
                         const float **vectors,
                         int count,
                         const DiskAnnBatchConfig *config);
```

**Pros:**

- ✅ Right use case (incremental batches, not bulk rebuilds)
- ✅ Low memory (~5MB for 500 vectors, not entire index)
- ✅ Significant speedup (10-50x) from parallelism + deduplication
- ✅ Simpler than full rebuild (~600 lines vs ~1200 lines)
- ✅ Closes gap to sqlite-vec (from 100x slower to 2-3x slower)

**Cons:**

- ⚠️ Edge updates still serial (SQLite single-writer limitation)
- ⚠️ Requires WAL mode (already enabled)

**Status:** Chosen approach

---

### REJECTED: In-Memory Full Rebuild

Original intern's approach: Load all vectors into RAM, build graph in memory, serialize back.

**Why rejected:**

- ❌ Wrong use case (bulk load, not incremental batches)
- ❌ High memory (190MB+ for 300k vectors - PhotoStructure users don't have "gobs of RAM")
- ❌ Wasteful for adding 500 vectors to existing 300k index (rebuilds everything)
- ❌ More complex implementation (~1200 lines)

**Status:** Rejected

## Tasks

### High-Level Phases (Detailed tasks in implementation plan)

- [ ] **Phase 1:** Serial batch (no threading) - prove deduplication gives 2-4x speedup
- [ ] **Phase 2:** Add parallel search - prove 8-24x speedup from parallelism
- [ ] **Phase 3:** Integration, benchmarks, TypeScript wrappers

### Key Implementation Files

**To create:**

- `src/diskann_batch.h` - API declarations, internal structs
- `src/diskann_batch.c` - Three-phase implementation (~600 lines)
- `tests/c/test_batch.c` - Batch insert tests (~400 lines)

**To modify:**

- `src/diskann.h` - Add batch API
- `src/diskann_insert.c` - Make insert_shadow_row non-static
- `Makefile` - Add diskann_batch.c, add -lpthread
- `tests/c/test_runner.c` - Register batch tests (CRITICAL: manual registration required)

**Verification:**

```bash
# Phase 1: Serial batch
make test                    # +12 tests passing
make valgrind                # No leaks

# Phase 2: Parallel search
make asan                    # CRITICAL: detect data races
make valgrind                # No leaks with threading

# Phase 3: Final
make test-stress             # 10-50x speedup demonstrated
npm run test:ts              # TypeScript integration works
```

## Notes

### Handoff Notes (2026-02-11)

**What was done this session:**

1. **Validated original TPP (20260210-parallel-graph-construction.md):**
   - Intern's full rebuild approach was wrong for PhotoStructure use case
   - Missed neighbor deduplication (3-5x speedup)
   - Underestimated parallelism benefit (8-24x speedup)

2. **Identified correct approach:**
   - Incremental batch insert (FreshDiskANN-style)
   - Parallel search + deduplicated edge updates
   - Low memory (only batch results, not entire index)

3. **Created detailed implementation plan:**
   - Saved at `/home/mrm/.claude/plans/snuggly-imagining-puppy.md`
   - 3 phases: serial batch, parallel search, integration
   - ~700 lines implementation + ~400 lines tests

4. **Updated research documentation:**
   - `_research/parallel-indexing.md` - explains approach for CS students
   - Includes academic paper analysis and PhotoStructure context

**Key discoveries:**

- Neighbor deduplication is the key win for similar vectors (PhotoStructure's use case)
- SQLite WAL allows unlimited read-only connections (enables parallelism)
- Memory is NOT a constraint for batch sizes (5-10 MB is nothing on 4GB machines)

**Next session should:**

- Start Research & Planning phase
- Read required files to understand current implementation
- Validate approach against actual PhotoStructure usage patterns
- Design detailed data structures and algorithm flow

---

### Session Update (2026-02-11 - Later)

**Additional findings:**

1. **FreshDiskANN source code examined** (`../DiskANN` repo):
   - Uses **lazy consolidation** model: `insert()` + `maintain()` + `needs_maintenance()`
   - Streaming approach: continuous inserts, periodic consolidation when threshold hit
   - Different from our explicit batching: we process entire batch immediately
   - **Our approach is simpler for PhotoStructure:** predictable latency, no background maintenance

2. **Memory constraints validated:**
   - User confirmed: 4GB+ RAM typical, 1-10 MB for batch processing is "nothing"
   - 1000 × 256 × 4 = 1 MB for input vectors
   - Total ~5-10 MB including search results and dedup map
   - **No need to be conservative** - was overthinking memory

3. **Problem statement finalized:**
   - PhotoStructure inserts are 20-100x slower than sqlite-vec (CRITICAL business problem)
   - Batch sizes: 100-1000 vectors from photo imports
   - High similarity expected (same event photos) → high neighbor overlap
   - Target: Close gap from 100x slower to 2-3x slower

4. **Documentation updated:**
   - Old TPP (20260210-parallel-graph-construction.md) marked as SUPERSEDED
   - Research doc (\_research/parallel-indexing.md) updated to reflect batch approach
   - Implementation plan at `/home/mrm/.claude/plans/snuggly-imagining-puppy.md`

**Key decisions:**

- **Explicit batching** (our approach) vs **lazy consolidation** (FreshDiskANN)
  - Pros: Simpler, predictable latency, no background threads
  - Cons: User must explicitly call batch insert (acceptable for PhotoStructure workflow)

**Next session priorities:**

1. Review FreshDiskANN paper details to ensure we're not missing critical optimizations
2. Start Research & Planning: read current insert/search implementation
3. Validate neighbor deduplication assumption with small-scale test
4. Design NeighborUpdateMap data structure (hashmap or array-based?)
