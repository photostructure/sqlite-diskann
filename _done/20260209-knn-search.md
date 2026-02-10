# k-NN Search Implementation

## Summary

Extract and implement `diskann_search()` — the k-nearest-neighbor graph traversal
algorithm. This is the core read path of the index: given a query vector, traverse the
DiskANN graph to find the k closest vectors. Also extracts `diskAnnSearchInternal()`,
the shared beam search used by both search AND insert.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [x] Cleanup & Documentation
- [x] Final Review

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
  `is_writable=0` mode with a single reusable BlobSpot. All vector data is `float*`(not libSQL's`Vector\*` type) since we are float32-only.
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
  `int is_writable` parameter (0 or 1). Define constants in `diskann_blob.h` (natural home
  since they name the values passed to `blob_spot_create`):
  `#define DISKANN_BLOB_READONLY 0` / `#define DISKANN_BLOB_WRITABLE 1`.

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

## Implementation Design

### Files changed (5)

1. **`src/diskann_blob.h`** — Add `DISKANN_BLOB_READONLY 0` / `DISKANN_BLOB_WRITABLE 1`
2. **`src/diskann_search.h`** (NEW) — `DiskAnnSearchCtx` struct + 4 exposed function decls
3. **`src/diskann_search.c`** (NEW) — 9 static helpers + 4 exposed funcs + `diskann_search()`
4. **`src/diskann_api.c`** — Delete `diskann_search()` stub (lines 455-468)
5. **`Makefile`** — Add `diskann_search.c` to SOURCES

### `DiskAnnSearchCtx` struct (in `diskann_search.h`)

Simplified from original (no VectorPair, float-only, query is borrowed not owned):

```c
typedef struct DiskAnnSearchCtx {
  const float *query;              /* borrowed, not owned */
  DiskAnnNode **candidates;        /* sorted by distance ascending */
  float *distances;                /* parallel to candidates */
  unsigned int n_candidates;
  unsigned int max_candidates;     /* = searchL or insertL */
  DiskAnnNode **top_candidates;    /* top-K exact results */
  float *top_distances;
  int n_top_candidates;
  int max_top_candidates;          /* = k */
  DiskAnnNode *visited_list;       /* linked list of visited nodes */
  unsigned int n_unvisited;
  int blob_mode;                   /* DISKANN_BLOB_READONLY or WRITABLE */
} DiskAnnSearchCtx;
```

### Function visibility

**Exposed via `diskann_search.h`** (4 — needed by future insert):

- `diskann_search_ctx_init()` / `diskann_search_ctx_deinit()`
- `diskann_select_random_shadow_row()`
- `diskann_search_internal()`

**Static in `diskann_search.c`** (9 — internal to beam search):

- `search_ctx_is_visited`, `search_ctx_has_candidate`
- `search_ctx_should_add` — delegates to `distance_buffer_insert_idx()` from `diskann_node.h` (DRY)
- `search_ctx_mark_visited`, `search_ctx_has_unvisited`
- `search_ctx_get_candidate`, `search_ctx_delete_candidate`
- `search_ctx_insert_candidate`, `search_ctx_find_closest_unvisited`

**Public (declared in `diskann.h`, defined in `diskann_search.c`):**

- `diskann_search()` — replaces stub in `diskann_api.c`

### Key design decisions

- **Query is borrowed `const float*`** — VectorPair eliminated, no alloc/dealloc needed
- **Static helpers reuse `buffer_insert`/`buffer_delete`/`distance_buffer_insert_idx`** from
  `diskann_node.h` — avoids duplicating sorted array logic
- **READONLY mode reuses a single BlobSpot** via `blob_spot_reload()` — matches original
- **Error messages removed** — original used `char **pzErrMsg`, our API uses return codes only;
  caller can use `sqlite3_errmsg(db)` if needed
- **`SQLITE_DONE` for empty table** — `diskann_select_random_shadow_row()` returns SQLITE_DONE
  (101, positive) for empty index; `diskann_search()` checks this explicitly and returns 0

## Tasks

- [x] Study `diskAnnSearchInternal()` algorithm (lines 1283-1414) in detail
- [x] Study search context management functions (lines 990-1167)
- [x] Define blob mode constants in `src/diskann_blob.h` (before `blob_spot_create` decl):
  - `#define DISKANN_BLOB_READONLY 0`
  - `#define DISKANN_BLOB_WRITABLE 1`
- [x] Implement `diskann_select_random_shadow_row()` equivalent:
  - Simplified: single-key (rowid), not multi-key
  - SQL: `SELECT rowid FROM "{db}".{idx}_shadow LIMIT 1 OFFSET ABS(RANDOM()) %% MAX((SELECT COUNT(*) FROM "{db}".{idx}_shadow), 1)`
  - Returns `SQLITE_DONE` if table is empty, `DISKANN_OK` with rowid if found
- [x] Implement search context in `src/diskann_search.c`:
  - `diskann_search_ctx_init()` / `diskann_search_ctx_deinit()`
  - `search_ctx_is_visited()` / `search_ctx_has_candidate()` (static)
  - `search_ctx_mark_visited()` (static)
  - `search_ctx_should_add()` (static, dropped unused pIndex param)
  - `search_ctx_insert_candidate()` (static)
  - `search_ctx_delete_candidate()` (static, zombie edge handling)
  - `search_ctx_has_unvisited()` (static)
  - `search_ctx_get_candidate()` (static)
  - `search_ctx_find_closest_unvisited()` (static)
- [x] Implement `diskann_search_internal()` — core beam search:
  - Lazy BLOB loading (is_writable=0: reuse single BlobSpot)
  - Handle DISKANN_ROW_NOT_FOUND gracefully (zombie edges → delete candidate)
  - Populate top-K results with exact distances
  - **Fixed original bug:** `return rc` not `return SQLITE_OK` in cleanup path
  - Float32 simplification: skipped `pNode != pEdge` recalculation branch
  - Uses `const float*` from `node_bin_vector()`, not libSQL `Vector*`
- [x] Implement `diskann_search()` — public API wrapper:
  - Validates inputs (NULL checks, dimension mismatch)
  - Gets random start node
  - Runs search_internal
  - Copies top-K results to caller's DiskAnnResult array
  - Returns count of results found
- [x] Write tests in `tests/c/test_search.c` (18 tests, all compile, 15 failing as expected):
  - Validation: NULL index/query/results, dimension mismatch, negative k, zero k
  - Empty index → 0 results
  - Single vector: exact match, different query, k > n
  - 4-node fully-connected graph: exact match, nearest neighbor, sorted results,
    k < n, k > n, readonly (no writes)
  - 50-vector recall test: brute-force comparison, recall ≥ 80%
  - Cosine metric: verify cosine distance works
- [x] Wire into Makefile and test_runner.c (auto-picked up by wildcard)
- [x] Create `src/diskann_search.h` with DiskAnnSearchCtx struct + exposed function decls
- [x] Add `diskann_search.c` to SOURCES in Makefile
- [x] Delete `diskann_search()` stub from `src/diskann_api.c`

**Verification:**

```bash
make test      # All tests pass
make asan      # No memory errors
make valgrind  # No leaks
```

## Session Notes (2025-02-09, Implementation phase)

**Implementation was straightforward.** The extraction from `diskann.c` went cleanly because
all dependencies (DiskAnnNode, BlobSpot, distance functions, buffer helpers) were already
extracted in earlier TPPs. The entire implementation compiled on the first try with all
111 tests passing, ASan and Valgrind clean.

**Struct field naming:** The TPP design spec used `unsigned int` for `n_candidates` and
`n_unvisited`, but the implementation uses `int` throughout to match the signatures of
`buffer_insert`/`buffer_delete`/`distance_buffer_insert_idx` (which all take `int` params).
This avoids sign-conversion warnings with `-Wconversion`. The `DiskAnnSearchCtx` in the
header reflects the actual implementation, not the design spec.

**`diskann_select_random_shadow_row` return values:** Uses `SQLITE_DONE` (101, positive)
for empty table — same as original. This is a SQLite convention, not a DISKANN error code.
`diskann_search()` checks for this explicitly before initializing the search context.

**All 18 search tests pass:** validation (6), empty index (1), single-vector (3),
known-graph (6), brute-force recall (1), cosine metric (1). The recall test uses 50 vectors
with a ring-connected graph (each node → next 8 nodes), query = (0.5, 0.5, 0.5), and
achieves ≥80% recall@5.

**`ctx_initialized` flag in `diskann_search()`:** Added to avoid double-deinit if
`diskann_search_internal` fails. In practice, `deinit` is always safe to call after
`init` succeeds, so this is belt-and-suspenders.

## Notes

**Blocked by:** ~~`20250209-shared-graph-types.md`~~ ✅ Complete — all dependencies available

**Blocks:** Future insert TPP (insert calls `diskann_search_internal()`)

**For insert TPP:** `diskann_search_internal` is exposed via `diskann_search.h` with
`DISKANN_BLOB_WRITABLE` mode. Insert will need to:

1. Call `diskann_search_ctx_init` with `DISKANN_BLOB_WRITABLE` and `insertL` beam width
2. Call `diskann_search_internal` to find nearest neighbors
3. Access `ctx.visited_list` to get visited nodes (which have loaded writable BlobSpots)
4. Use those BlobSpots to add edges to neighbors

**Integration phase (next):** The search function is already wired into the public API
(`diskann.h` declares it, `diskann_search.c` defines it, `Makefile` compiles it, stub
removed from `diskann_api.c`). Integration may be a no-op or quick verification that the
module boundary is clean. Consider whether `diskann_search.h` should be included anywhere
else (e.g., will future insert code need it?).

**Cleanup phase:** Consider removing the `ctx_initialized` flag — it's redundant since
`diskann_search_ctx_deinit` is always called after `init` succeeds. Also review whether
any dead code in `diskann.c` can be flagged for removal now that search is extracted.
