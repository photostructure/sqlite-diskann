# k-NN Search Implementation

## Summary

Extract and implement `diskann_search()` — the k-nearest-neighbor graph traversal
algorithm. This is the core read path of the index: given a query vector, traverse the
DiskANN graph to find the k closest vectors. Also extracts `diskAnnSearchInternal()`,
the shared beam search used by both search AND insert.

## Current Phase

- [x] Research & Planning
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
- `src/diskann.c` lines 990-1050 - Search context init/deinit
- `src/diskann.c` lines 1054-1167 - Search context helpers (visited, candidates, queue)
- `src/diskann.c` lines 1283-1414 - `diskAnnSearchInternal()` — CORE beam search
- `src/diskann.c` lines 1421-1490 - `diskAnnSearch()` — public search entry point
- `src/diskann.c` lines 625-662 - `diskAnnSelectRandomShadowRow()` — entry point selection
- `src/diskann.h` - Public API (diskann_search signature)
- `src/diskann_blob.h` - BlobSpot API (used for lazy BLOB loading)
- `src/diskann_internal.h` - DiskAnnIndex struct

## Description

- **Problem:** `diskann_search()` is currently a stub returning `DISKANN_ERROR`. The
  core beam search algorithm in `diskAnnSearchInternal()` is trapped in the coupled
  `diskann.c` with libSQL dependencies.
- **Constraints:** Must use standalone types from `20250209-shared-graph-types.md`
  (DiskAnnNode, DiskAnnSearchCtx, distance functions, nodeBin* helpers). Must use
  existing BlobSpot layer for BLOB I/O. Search is read-only — uses
  `is_writable=0` mode with a single reusable BlobSpot. All vector data is `float*`
  (not libSQL's `Vector*` type) since we are float32-only.
- **Success Criteria:**
  - `diskann_search()` returns correct k-NN results on small test datasets
  - Results match brute-force reference for known vectors
  - Recall > 95% on 1000-vector random dataset
  - Read-only: no BLOB writes during search
  - ASan + Valgrind clean
  - Performance: <1ms for k=10 on 1000 vectors

## Tribal Knowledge

**The beam search algorithm (`diskAnnSearchInternal`):**
1. Start from a random node (or medoid if stored)
2. Maintain sorted candidate queue (by approximate distance, ascending)
3. Pop closest unvisited candidate, load its BLOB, read its edges
4. For each edge: compute distance, insert into queue if promising
5. Queue has fixed budget (`searchL` parameter) — drops furthest when full
6. Maintain separate top-K list with exact distances (from node vectors, not edge vectors)
7. Stop when no unvisited candidates remain

**Read-only vs writable blob mode:**
- Search uses `is_writable=0`: single `BlobSpot` reused via `blob_spot_reload()` for each
  candidate. Memory-efficient — only one BLOB buffer alive at a time.
- Insert uses `is_writable=1`: separate `BlobSpot` per visited node (needs to write edges later).
- This is controlled by `blob_mode` field in `DiskAnnSearchCtx`.
- The original libSQL code used `DISKANN_BLOB_READONLY` / `DISKANN_BLOB_WRITABLE` constants
  from internal headers. Our extracted `blob_spot_create()` in `diskann_blob.h` uses a plain
  `int is_writable` parameter (0 or 1). We should define constants in `diskann_node.h` for
  clarity: `#define DISKANN_BLOB_READONLY 0` / `#define DISKANN_BLOB_WRITABLE 1`.

**Approximate vs exact distance:**
- Edge vectors may be compressed (lower precision than node vectors)
- Queue ordering uses approximate distance (from edge vectors) — fast but imprecise
- Top-K list uses exact distance (from full node vectors) — accurate results
- For float32-only (no compression), these are identical

**Random start node:** `diskAnnSelectRandomShadowRow()` picks a random row using:
```sql
SELECT rowid FROM shadow LIMIT 1 OFFSET ABS(RANDOM()) % MAX((SELECT COUNT(*) FROM shadow), 1)
```
This avoids `ORDER BY RANDOM()` (which sorts the whole table) but is still O(n) due to the
`COUNT(*)` subquery plus the OFFSET scan. Consider caching or storing a medoid entry point
in metadata for large indexes.

**Empty index:** If shadow table has zero rows, search returns 0 results (not an error).
First check with `diskAnnSelectRandomShadowRow()` which returns `SQLITE_DONE` if empty.

**Search context memory management:**
- `aCandidates` and `aDistances` are parallel `float` arrays, allocated to `maxCandidates` size
  - **Bug in original:** libSQL allocates these with `sizeof(double)` but uses as `float*`.
    Our extraction must use `sizeof(float)` consistently.
- `aTopCandidates` and `aTopDistances` are parallel `float` arrays, allocated to `maxTopCandidates`
  - Same `sizeof(double)` bug applies here.
- `aCandidates` contains BOTH visited and unvisited candidates (not just unvisited).
  Visited candidates remain in the array but are skipped by `find_closest_unvisited()`.
  The deinit function only frees unvisited candidates from the array (visited ones are
  freed via the `visitedList`).
- `visitedList` is a singly-linked list of DiskAnnNode pointers
- Context owns all nodes put into it (frees them in deinit)
- Candidates array is sorted by distance — binary-ish insertion via `distanceBufferInsertIdx`

**Zombie edge handling:** If a node has been deleted, `blob_spot_reload()` returns
`DISKANN_ROW_NOT_FOUND`. `diskann_search_internal` must handle this gracefully — delete
the candidate from the queue (via `diskann_search_ctx_delete_candidate()`) and continue.
This is how delete's "conservative no-repair" strategy works.

**Bug in original `diskAnnSearchInternal` (line 1413):** The `out:` label always returns
`SQLITE_OK`, even when `rc` was set to an error (NOMEM, blob failure, etc.). The `goto out`
paths set `rc` but the return ignores it. Our extracted version MUST fix this: `return rc;`.

**`diskAnnSearchCtxShouldAddCandidate` unused parameter:** The original takes
`const DiskAnnIndex *pIndex` but never uses it. Our extracted version should drop this
parameter.

**Float32 simplification:** The original code uses libSQL's `Vector*` type throughout.
`nodeBinVector()` returns a `Vector*`, `diskAnnVectorDistance()` takes `Vector*`, and
`VectorPair` holds separate node/edge vector representations for compression support.
In our float32-only extraction: `node_bin_vector()` returns `float*` (zero-copy pointer
into BLOB buffer), `diskann_distance()` takes `float*`, and `VectorPair` is eliminated
entirely (node and edge vectors are the same type). The `query.pNode != query.pEdge`
check (line 1366) is always false for float32-only, so the exact-distance recalculation
branch can be omitted.

## Solutions

### Option 1: Extract into `src/diskann_search.c` ⭐ CHOSEN
**Pros:** Clean separation, search is a natural module boundary
**Cons:** Shares `diskAnnSearchInternal` with insert (future)
**Status:** Chosen — expose searchInternal via internal header for insert to use later

### Option 2: Keep search in `diskann_api.c`
**Pros:** Fewer files
**Cons:** `diskann_api.c` would grow huge, mixes lifecycle + algorithm code
**Status:** Rejected

## Tasks

- [x] Study `diskAnnSearchInternal()` algorithm (lines 1283-1414) in detail
- [x] Study search context management functions (lines 990-1167)
- [ ] Define blob mode constants in `src/diskann_node.h`:
  - `#define DISKANN_BLOB_READONLY 0`
  - `#define DISKANN_BLOB_WRITABLE 1`
- [ ] Implement `diskann_select_random_shadow_row()` equivalent:
  - Simplified: single-key (rowid), not multi-key
  - SQL: `SELECT rowid FROM "{db}".{idx}_shadow LIMIT 1 OFFSET ABS(RANDOM()) %% MAX((SELECT COUNT(*) FROM "{db}".{idx}_shadow), 1)`
  - Returns `SQLITE_DONE` if table is empty, `SQLITE_OK` with rowid if found
- [ ] Implement search context in `src/diskann_search.c`:
  - `diskann_search_ctx_init()` / `diskann_search_ctx_deinit()`
  - `diskann_search_ctx_is_visited()` / `diskann_search_ctx_has_candidate()`
  - `diskann_search_ctx_mark_visited()`
  - `diskann_search_ctx_should_add_candidate()` (drop unused `pIndex` param from original)
  - `diskann_search_ctx_insert_candidate()`
  - `diskann_search_ctx_delete_candidate()` (needed for zombie edge handling)
  - `diskann_search_ctx_has_unvisited()`
  - `diskann_search_ctx_get_candidate()`
  - `diskann_search_ctx_find_closest_unvisited()`
- [ ] Implement `diskann_search_internal()` — core beam search:
  - Lazy BLOB loading (is_writable=0: reuse single BlobSpot)
  - Handle DISKANN_ROW_NOT_FOUND gracefully (zombie edges → delete candidate)
  - Populate top-K results with exact distances
  - **Fix original bug:** `return rc` not `return SQLITE_OK` in cleanup path
  - Float32 simplification: skip `pNode != pEdge` recalculation branch (always same)
  - Use `float*` from `node_bin_vector()`, not libSQL `Vector*`
- [ ] Implement `diskann_search()` — public API wrapper:
  - Validate inputs (NULL checks, dimension mismatch)
  - Get random start node
  - Run search_internal
  - Copy top-K results to caller's DiskAnnResult array
  - Return count of results found
- [ ] Write tests in `tests/c/test_search.c`:
  - Search empty index → 0 results
  - Search with NULL/invalid args → error codes
  - Search 1-vector index → returns that vector
  - Search known 10-vector dataset → correct nearest neighbor
  - Search with k > n → returns n results
  - Brute-force comparison on 100-vector random dataset
- [ ] Expose `diskann_search_internal()` via `src/diskann_internal.h` for future insert use
- [ ] Wire into Makefile and test_runner.c

**Verification:**
```bash
make test      # All tests pass
make asan      # No memory errors
make valgrind  # No leaks
```

## Notes

**Blocked by:** `20250209-shared-graph-types.md` (needs DiskAnnNode, DiskAnnSearchCtx,
distance functions, nodeBin* helpers)

**Blocks:** Future insert TPP (insert calls `diskann_search_internal()`)

**To populate test data:** Tests need to manually insert BLOBs into the shadow table
using the nodeBin* functions from the shared types TPP. Can't use `diskann_insert()`
since it doesn't exist yet. Write a test helper that constructs node BLOBs directly.
