# Parallel Graph Construction for DiskANN

## Summary

Enable parallel insertion of vectors by adding a two-phase bulk-load API: `diskann_insert_vector()` stores vectors without graph construction, then `diskann_build()` constructs the Vamana graph in RAM using pthreads and serializes to SQLite. Achieves 5-10x speedup over serial `diskann_insert()` for batch workloads.

## Current Phase

- [x] Research & Planning
- [x] Test Design (documented in TPP; test files not yet created)
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
- `src/diskann_insert.c` - Current serial insertion (replace_edge_idx, prune_edges)
- `src/diskann_search.c` - Beam search (diskann_search_internal) — port to in-memory
- `src/diskann_node.h` - V3 binary format, distance functions (reuse directly)
- `src/diskann_blob.c` - BLOB I/O layer (used for load/serialize)
- `src/diskann_internal.h` - DiskAnnIndex struct

## Description

**Problem:** Current DiskANN insertion is serial — each vector must be inserted one at a time because graph construction depends on the existing graph state. With 24 CPU cores, we're only using 4-5% capacity during bulk imports (300k vectors takes ~30 minutes).

**Root Cause:** The bottleneck is graph construction (beam search + edge mutations), NOT SQLite I/O. Each `diskann_insert()` does: SAVEPOINT → beam search (~200 node reads) → add edges to new node → update neighbor edges (with blob_spot_flush per neighbor) → RELEASE. Every insert mutates the graph, so the next insert sees a different graph state.

**Constraints:**

- Must maintain graph quality (recall@10 >= 75%)
- SQLite: single writer per database (WAL allows concurrent readers)
- Memory budget: <2GB for 300k @ 92D

**Success Criteria:**

- 300k vector import completes in <5 minutes (6x speedup)
- Recall@10 remains >= 75%
- Memory usage < 2GB for 300k @ 92D (~190MB estimated)
- Thread-safe implementation
- ASan + Valgrind clean

## Tribal Knowledge

### Current Serial Insert Path

```
diskann_insert(idx, id, vector, dims)
  1. SELECT random start node (diskann_select_random_shadow_row)
  2. BEGIN SAVEPOINT diskann_insert_{name}
  3. diskann_search_internal() — beam search from random start
     └─ Opens BlobSpots in WRITABLE mode
     └─ Traverses graph reading neighbor lists
  4. INSERT INTO shadow (id, zeroblob(block_size))
  5. blob_spot_create + reload + node_bin_init — write vector
  6. Phase 1: For each visited node, replace_edge_idx + prune_edges on NEW node
  7. Phase 2: For each visited node, replace_edge_idx + prune_edges on NEIGHBOR
     └─ blob_spot_flush per neighbor (expensive!)
  8. blob_spot_flush new node
  9. RELEASE SAVEPOINT (or ROLLBACK on error)
```

**Key bottlenecks:**

- Beam search is single-threaded (lines 211-224 of diskann_insert.c)
- Phase 2 edge updates are serial — each neighbor BLOB flushed sequentially
- SAVEPOINT wraps entire operation — enforces atomicity but prevents parallelism
- Writable BLOB handles hold reserved locks on the database

### SQLite Concurrency Constraints

- WAL mode: unlimited readers + 1 writer concurrently
- Each thread CAN open its own read-only connection for parallel search
- Writes must be serialized through one connection
- Writable BLOB handles hold reserved locks until closed
- SAVEPOINT creates nested transaction — serializes other transactions
- The `sqlite3 *db` in DiskAnnIndex is borrowed and single-threaded

### What Can Be Parallelized

| Component                    | Parallelizable?        | Why                                           |
| ---------------------------- | ---------------------- | --------------------------------------------- |
| Beam search (read-only)      | YES (multi-connection) | Each search context is isolated               |
| Phase 1 (edges to new node)  | YES (separate nodes)   | Different new nodes don't conflict            |
| Phase 2 (edges to neighbors) | NO (directly)          | Multiple inserts update overlapping neighbors |
| Shadow row INSERT            | YES (batch)            | Single multi-row INSERT                       |
| Distance calculations        | YES (precompute)       | Pure math, no state                           |

### Functions to Port to In-Memory

| SQLite-backed (existing)    | In-memory (new)            | Source location               |
| --------------------------- | -------------------------- | ----------------------------- |
| `diskann_search_internal()` | `memgraph_greedy_search()` | `src/diskann_search.c`        |
| `replace_edge_idx()`        | `memgraph_replace_edge()`  | `src/diskann_insert.c:30-73`  |
| `prune_edges()`             | `memgraph_prune_edges()`   | `src/diskann_insert.c:86-123` |
| `diskann_distance()`        | **Reuse directly**         | `src/diskann_node.c`          |

Distance functions (`diskann_distance_l2`, `diskann_distance_cosine`) operate on raw `float*` arrays — already usable without modification.

### insert_shadow_row() Visibility

Currently `static` in `diskann_insert.c`. Needed by both `diskann_insert()` and new `diskann_insert_vector()`. Either make non-static and declare in `diskann_internal.h`, or keep both functions in `diskann_insert.c`.

## Solutions

### SELECTED: Two-Phase In-Memory Build (Vamana-style)

**Approach:**

1. **INSERT phase:** `diskann_insert_vector()` stores vectors in shadow table (no graph search, no edge mutations). Fast serial SQLite writes.
2. **BUILD phase:** `diskann_build()` loads all vectors into RAM, constructs Vamana graph in parallel using pthreads, serializes completed graph back to SQLite.

**Why this won over alternatives:**

| Factor            | In-Memory Build (selected)              | Separate SQLite DBs                 | Batch+Parallel Search |
| ----------------- | --------------------------------------- | ----------------------------------- | --------------------- |
| Speedup           | 5-10x                                   | 5-10x                               | 2-3x                  |
| Memory (300k×92D) | ~190 MB                                 | ~190 MB + N×disk I/O                | Minimal               |
| Merge step        | None                                    | Complex cross-partition edge repair | None                  |
| Graph quality     | Same as serial Vamana                   | Degraded at boundaries              | Same as serial        |
| Implementation    | Medium                                  | High                                | Low                   |
| Future-proof      | API compatible with partitioned backend | N/A                                 | Dead end              |

**Academic support:**

- **ParlayANN (PPoPP 2024):** Lock-free parallel DiskANN construction. Proves per-node mutexes are sufficient given low contention (~1/n probability of collision).
- **SOGAIC (ArXiv 2502.20695, 2025):** Production partition→build→merge at 10B scale. Validates architecture for future scaling.
- **Original DiskANN paper (NeurIPS 2019):** Explicitly describes building Vamana in RAM on overlapping partitions.
- **FreshDiskANN (2021):** Proves incremental high-throughput updates are achievable with DiskANN.

### Rejected: Separate SQLite DBs per Thread

The user's suggestion of pre-clustering vectors into separate SQLite databases on different threads, then merging. Clever but adds significant merge complexity for no benefit at this scale (300k vectors = ~190MB, easily fits in RAM). The merge step requires cross-partition edge repair which degrades graph quality at boundaries. Worth revisiting if dataset exceeds RAM (~2M+ vectors at 256D).

### Rejected: Batch + Parallel Neighbor Search (Option 2)

Buffer N vectors, parallel neighbor search, serial edge writes. Limited to 2-3x speedup because edge mutations (Phase 2) are the actual bottleneck, not search. Dead-end architecture.

## New C API

```c
/* Store vector without graph construction (fast) */
int diskann_insert_vector(DiskAnnIndex *idx, int64_t id,
                          const float *vector, uint32_t dims);

/* Progress callback */
typedef void (*diskann_progress_fn)(int vectors_processed, int total_vectors,
                                    void *user_data);

/* Build configuration */
typedef struct DiskAnnBuildConfig {
  int num_threads;                /* 0 = auto-detect (all cores) */
  diskann_progress_fn progress;   /* NULL = no progress reporting */
  void *progress_user_data;       /* passed to progress callback */
} DiskAnnBuildConfig;

/* Build graph from all stored vectors (parallel) */
int diskann_build(DiskAnnIndex *idx, const DiskAnnBuildConfig *config);
```

Existing `diskann_insert()` remains for incremental single-vector insertion. Incremental inserts work naturally on a graph built by `diskann_build()`.

## In-Memory Graph Design

### Data Structures

```c
/* src/diskann_memgraph.h */
typedef struct MemNode {
  int32_t edge_count;
  int32_t *edge_ids;    /* neighbor indices (internal 0..n-1, NOT rowids) */
  float *edge_dists;    /* distance to each neighbor */
  pthread_mutex_t lock; /* per-node lock for edge mutations */
} MemNode;

typedef struct MemGraph {
  float *vectors;       /* flat [n_vectors * dims], row-major, cache-friendly */
  int64_t *rowids;      /* maps internal index -> user-provided ID */
  MemNode *nodes;       /* graph adjacency lists */
  int32_t n_vectors;
  uint32_t dims;
  uint32_t max_neighbors;
  uint8_t metric;
  double pruning_alpha;
} MemGraph;
```

### Memory Budget (300k × 92D)

- Vectors: 300,000 × 92 × 4 bytes = 105 MB
- Rowids: 300,000 × 8 bytes = 2.3 MB
- Nodes (32 neighbors): 300,000 × (4 + 32×4 + 32×4 + ~40 mutex) = ~80 MB
- **Total: ~190 MB** (well within 2GB budget)

### Parallel Vamana Algorithm

Two passes for quality (standard Vamana):

```
Per pass:
  1. Fisher-Yates shuffle all node indices (deterministic seed)
  2. Split into chunks (1 per thread)
  3. Each thread processes its chunk:
     for node_idx in my_chunk:
       a. Greedy search in-memory graph for L nearest neighbors
          (lock-free reads — stale edges are fine, Vamana is robust)
       b. Lock node_idx → RobustPrune edges → unlock
       c. For each new neighbor:
          Lock neighbor → add back-edge (prune if full) → unlock
```

**Why per-node mutexes, not lock-free:** Lock-free CAS on variable-length adjacency lists is complex. Per-node mutexes are simpler, correct, and fast — contention probability is ~1/n (e.g., 1/300,000). Can optimize to lock-free later if profiling shows contention.

### Data Flow

```
1. INSERT phase (serial, ~1 min for 300k):
   diskann_insert_vector() × N
   → INSERT INTO shadow (id, zeroblob(block_size))
   → Write vector into BLOB (node_bin_init with zero edges)
   → No graph search, no edge mutations

2. BUILD phase (parallel, ~1-2 min with 16-24 threads):
   diskann_build(idx, config)
   → memgraph_create() — allocate MemGraph
   → memgraph_load_vectors() — SELECT all from shadow, load into flat float[]
   → memgraph_init_random_edges() — seed 3 random edges per node
   → memgraph_build_pass() × 2 — parallel Vamana with pthreads
   → memgraph_serialize() — write edges back to shadow BLOBs in one txn
   → memgraph_destroy() — free all memory
```

## Files to Create/Modify

| File                      | Action     | Description                                                                                   |
| ------------------------- | ---------- | --------------------------------------------------------------------------------------------- |
| `src/diskann.h`           | **Modify** | Add `diskann_insert_vector()`, `diskann_progress_fn`, `DiskAnnBuildConfig`, `diskann_build()` |
| `src/diskann_memgraph.h`  | **New**    | MemGraph/MemNode structs, all function declarations                                           |
| `src/diskann_memgraph.c`  | **New**    | create, destroy, load_vectors, greedy_search, robust_prune, serialize                         |
| `src/diskann_build.c`     | **New**    | `diskann_build()` orchestrator, pthread worker, Fisher-Yates shuffle                          |
| `src/diskann_insert.c`    | **Modify** | Add `diskann_insert_vector()`                                                                 |
| `Makefile`                | **Modify** | Add new .c to SOURCES, add `-lpthread` to LIBS                                                |
| `tests/c/test_build.c`    | **New**    | Tests for insert_vector + build (correctness, recall, multi-thread)                           |
| `tests/c/test_memgraph.c` | **New**    | Unit tests for in-memory graph operations                                                     |
| `tests/c/test_stress.c`   | **Modify** | Add parallel build benchmark alongside serial                                                 |
| `src/index.ts`            | **Modify** | Add `insertVectorOnly()`, `buildIndex()` TypeScript wrappers                                  |
| `src/types.ts`            | **Modify** | Add `DiskAnnBuildOptions` interface                                                           |

## Test Design

### test_memgraph.c — In-Memory Graph Unit Tests

Tests the MemGraph data structure and algorithms in isolation (no SQLite, no threading).

**Lifecycle & validation:**

- `test_memgraph_create_destroy` — create with valid params, verify fields, destroy without leak
- `test_memgraph_create_zero_vectors` — n_vectors=0 returns error
- `test_memgraph_create_zero_dims` — dims=0 returns error
- `test_memgraph_destroy_null` — NULL is safe no-op

**Vector loading:**

- `test_memgraph_load_vectors_small` — load 10 vectors from shadow table, verify flat array contents match
- `test_memgraph_load_vectors_rowid_mapping` — verify rowids[] maps internal indices to user IDs correctly
- `test_memgraph_load_vectors_empty_index` — empty shadow table returns n_vectors=0

**Greedy search (in-memory):**

- `test_memgraph_search_single_node` — 1 node, search returns it
- `test_memgraph_search_linear_chain` — 5 nodes in a chain (A→B→C→D→E), search from A finds E
- `test_memgraph_search_fully_connected` — 4 fully-connected nodes, exact match returns correct node
- `test_memgraph_search_returns_sorted` — results sorted by distance ascending

**Edge replacement (port of replace_edge_idx):**

- `test_memgraph_replace_edge_append` — append when under max_neighbors
- `test_memgraph_replace_edge_dominated` — dominated edge returns -1 (skip)
- `test_memgraph_replace_edge_replaces_worst` — replaces farthest existing edge
- `test_memgraph_replace_edge_duplicate` — duplicate rowid replaces in-place

**Edge pruning (port of prune_edges):**

- `test_memgraph_prune_removes_dominated` — dominated edges removed
- `test_memgraph_prune_keeps_diverse` — diverse edges kept
- `test_memgraph_prune_preserves_minimum` — at least 1 edge always remains

**Random edge initialization:**

- `test_memgraph_init_random_edges` — all nodes get 1-3 edges after init
- `test_memgraph_init_random_edges_no_self` — no self-loops

**Serialization (MemGraph → SQLite BLOBs):**

- `test_memgraph_serialize_roundtrip` — build graph, serialize, read back via BlobSpot, verify edges match
- `test_memgraph_serialize_edge_count` — serialized nodes have correct edge counts
- `test_memgraph_serialize_vectors_unchanged` — vector data in BLOBs unchanged after serialize

### test_build.c — Build API Tests

Tests `diskann_insert_vector()` and `diskann_build()` end-to-end.

**insert_vector validation:**

- `test_insert_vector_null_index` — NULL idx returns DISKANN_ERROR_INVALID
- `test_insert_vector_null_vector` — NULL vector returns DISKANN_ERROR_INVALID
- `test_insert_vector_dimension_mismatch` — wrong dims returns DISKANN_ERROR_DIMENSION
- `test_insert_vector_duplicate_id` — duplicate ID returns DISKANN_ERROR_EXISTS
- `test_insert_vector_stores_data` — verify shadow row + BLOB created with correct vector

**insert_vector + build basic workflow:**

- `test_build_single_vector` — 1 vector, build succeeds, searchable
- `test_build_two_vectors` — 2 vectors, build succeeds, mutual edges exist
- `test_build_ten_vectors` — 10 vectors, build succeeds, all searchable

**build validation:**

- `test_build_null_index` — NULL idx returns error
- `test_build_empty_index` — no vectors, returns error or no-op
- `test_build_null_config_uses_defaults` — NULL config auto-detects threads

**Correctness — recall comparison:**

- `test_build_recall_vs_serial_3d` — 50 vectors @ 3D: build recall >= serial insert recall
- `test_build_recall_vs_serial_128d` — 200 vectors @ 128D: build recall@10 >= 75%
- `test_build_recall_cosine` — verify works with cosine metric too

**Graph quality:**

- `test_build_all_nodes_have_edges` — every node has >= 1 edge after build
- `test_build_edge_count_within_limit` — no node exceeds max_neighbors
- `test_build_graph_connected` — BFS from any node reaches all others (connected graph)

**Incremental insert after build:**

- `test_build_then_incremental_insert` — build 50, then diskann_insert 5 more, all 55 searchable
- `test_build_then_search` — basic search works on built graph

**Threading (Phase 2):**

- `test_build_single_thread` — num_threads=1, verify correctness
- `test_build_multi_thread` — num_threads=4, verify same recall as single-thread
- `test_build_auto_thread` — num_threads=0, verify auto-detection works

**Progress callback:**

- `test_build_progress_callback` — verify callback is called with increasing values
- `test_build_progress_null_callback` — NULL callback doesn't crash

### test_stress.c additions — Parallel Build Benchmark

Add `test_stress_build_300k_92d` alongside existing `test_stress_300k_92d`:

- Uses `diskann_insert_vector()` × 300k + `diskann_build()` instead of `diskann_insert()` × 300k
- Same search/recall measurement as existing stress test
- Prints comparison: serial time vs parallel build time
- Asserts recall@10 >= 75% (slightly relaxed from serial's 80%)
- Asserts build time < serial time (speedup > 1x)

### Test Registration

**test_runner.c:** Add forward declarations + RUN_TEST for all test_memgraph and test_build tests.

**Makefile:** test*memgraph.c and test_build.c added via TEST_C_SOURCES wildcard (already matches `test*\*.c`).

## Tasks

### Phase 1: In-Memory Graph Foundation (no threading)

- [ ] Create `src/diskann_memgraph.h` with MemGraph/MemNode structs and function declarations
- [ ] Implement `memgraph_create(dims, max_neighbors, metric, pruning_alpha)` — allocate all arrays
- [ ] Implement `memgraph_destroy()` — free all memory, null pointers
- [ ] Implement `memgraph_load_vectors(graph, idx)` — SELECT from shadow table into flat float[]
- [ ] Implement `memgraph_greedy_search()` — port beam search from `diskann_search_internal()` to operate on flat arrays instead of BlobSpots
- [ ] Implement `memgraph_replace_edge()` — port from `replace_edge_idx()` to operate on MemNode
- [ ] Implement `memgraph_prune_edges()` — port from `prune_edges()` to operate on MemNode
- [ ] Implement `memgraph_init_random_edges()` — seed 3 random edges per node
- [ ] Implement `memgraph_serialize(graph, idx)` — write completed edges back to shadow BLOBs
- [ ] Write `tests/c/test_memgraph.c` — unit tests for all of the above
- [ ] Implement **serial** `diskann_build()` in `src/diskann_build.c` (single-threaded)
- [ ] Write `tests/c/test_build.c` — tests for serial build (correctness + recall vs serial insert)

### Phase 2: diskann_insert_vector() + Threading

- [ ] Add `diskann_insert_vector()` to `src/diskann_insert.c` (vector-only shadow row insert, no graph)
- [ ] Add `diskann_insert_vector()`, `diskann_progress_fn`, `DiskAnnBuildConfig`, `diskann_build()` to `src/diskann.h`
- [ ] Add pthread_mutex_t to MemNode, init/destroy in create/destroy
- [ ] Implement `memgraph_build_pass()` with pthread_create/join workers
- [ ] Implement Fisher-Yates shuffle (deterministic seed for reproducibility)
- [ ] Auto-detect core count via `sysconf(_SC_NPROCESSORS_ONLN)`
- [ ] Wire up progress callback (called from main thread after each batch)
- [ ] Add `-lpthread` to LIBS in Makefile, add new .c files to SOURCES
- [ ] Write thread-safety tests in `test_build.c` (multi-threaded build, verify no crashes)
- [ ] Run `make asan` — verify no data races or memory errors
- [ ] Run `make clean && make valgrind` — verify no leaks

### Phase 3: Integration + Benchmarking

- [ ] Update `tests/c/test_stress.c` — add parallel build benchmark alongside serial path
- [ ] Measure speedup: target 5-10x over serial `diskann_insert()`
- [ ] Measure recall@10: must be >= 75%
- [ ] Add TypeScript wrappers: `insertVectorOnly()`, `buildIndex()` in `src/index.ts`
- [ ] Add `DiskAnnBuildOptions` to `src/types.ts`
- [ ] Update TypeScript tests for new API
- [ ] Final ASan + Valgrind clean on all tests

**Verification:**

```bash
# Unit tests (includes test_memgraph + test_build)
make test

# Stress test (serial vs parallel comparison)
make test-stress

# Memory safety
make asan
make clean && make valgrind

# Recall quality (embedded in stress test output)
# Must show recall@10 >= 75%
```

## Decisions

- **Naming:** `diskann_insert_vector()` — parallels existing `diskann_insert()`
- **Progress callback:** Included in v1 via `diskann_progress_fn` in `DiskAnnBuildConfig`
- **Benchmarking:** Update existing `test_stress.c` with parallel build test case alongside serial
- **Build passes:** Hardcode 2 (standard Vamana). Can expose later if needed.
- **Incremental insert after build:** Works naturally — `diskann_insert()` beam-searches the built graph
- **Threading:** pthreads (not OpenMP) — explicit control, no compiler warning issues, portable

## Notes

**References:**

- Vamana/DiskANN paper: "DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node" (NeurIPS 2019)
- ParlayANN: "Scalable and Jointly Differentiable ANN" (PPoPP 2024) — lock-free parallel DiskANN
- SOGAIC: "Scalable Overload-Aware Graph Construction" (ArXiv 2502.20695, 2025) — partition+merge at 10B scale
- FreshDiskANN: "FreshDiskANN: A Fast and Accurate Graph-Based ANN Index for Streaming Similarity Search" (ArXiv 2105.09613, 2021)
- k-NN Graph Merging: "On the Merge of k-NN Graph" (ArXiv 1908.00814, 2019)
- SQLite WAL mode: https://sqlite.org/wal.html

**Current workaround:**

- Transaction batching + WAL mode gives 2-5x speedup (already implemented)
- This TPP is for the 5-10x additional speedup with parallel in-memory construction

**Future work (out of scope):**

- Partitioned build for datasets exceeding RAM (>2M vectors at 256D)
- Lock-free edge mutations (ParlayANN-style CAS)
- SIMD distance functions
- Medoid entry point instead of random start

## Handoff Notes (2025-02-10)

### What was done this session

1. **Research & Planning (complete):** Thorough analysis of the serial insert bottleneck, academic literature review (ParlayANN, SOGAIC, FreshDiskANN, original Vamana paper), SQLite concurrency analysis. Evaluated 3 approaches — in-memory Vamana build won decisively.

2. **Test Design (complete, documented only):** Full test design with 46 test cases across 3 files documented in the Test Design section above. Test stubs for `test_memgraph.c` and `test_build.c` were written but **rolled back** because the session exited plan mode prematurely. The full test code exists in the conversation history and in `/home/mrm/.claude/plans/melodic-dreaming-marble.md` — the next session can recreate them from the TPP test design section.

3. **No code changes committed.** The Makefile and test_runner.c edits were also rolled back.

### What the next session should do

The next TPP phase is **Implementation Design** but the design is already substantially documented in the "New C API", "In-Memory Graph Design", and "Files to Create/Modify" sections. The next session should:

1. Review the design sections and mark Implementation Design complete if satisfied
2. Move to **Test-First Development**: create `test_memgraph.c`, `test_build.c`, and update `test_runner.c` using the Test Design section as a spec
3. Create stub headers (`diskann_memgraph.h`) and stub source files (`diskann_memgraph.c`, `diskann_build.c`) so the test binary compiles but tests fail
4. Update Makefile: add new `.c` files to SOURCES, add `-lpthread` to LIBS
5. Note: `diskann_extension.c` was added to SOURCES since the TPP was started — the Makefile now has this file

### Key codebase notes for next session

- `insert_shadow_row()` is `static` in `diskann_insert.c` — simplest approach is to add `diskann_insert_vector()` in the same file so it can reuse it
- Distance functions in `diskann_node.h/.c` operate on raw `float*` — no changes needed for in-memory graph
- The beam search in `diskann_search.c` uses BlobSpot for node access — the in-memory port needs to replace BlobSpot reads with flat array indexing
- `replace_edge_idx()` and `prune_edges()` in `diskann_insert.c` are the two core algorithms that need in-memory equivalents — they're ~40 and ~40 lines respectively, well-contained
- TEST*C_SOURCES in Makefile uses `$(wildcard $(TEST_DIR)/c/test*\*.c)`with exclusions for`test_runner.c`and`test_stress.c` — new test files are auto-discovered

### No blockers

Everything needed to proceed is documented in this TPP. No external dependencies or unresolved questions.
