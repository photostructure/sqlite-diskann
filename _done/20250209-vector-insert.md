# Vector Insert Implementation

## Summary

Extract and implement `diskann_insert()` — add a vector to the DiskANN graph index.
The insert algorithm searches for nearby nodes, inserts a new shadow row, then builds
bidirectional edges with angle-based pruning. This is the core write path that constructs
the navigable graph.

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
- `src/diskann.c` lines 1176-1224 - `diskAnnReplaceEdgeIdx()` — edge replacement decision
- `src/diskann.c` lines 1229-1280 - `diskAnnPruneEdges()` — dominated edge removal
- `src/diskann.c` lines 1493-1623 - `diskAnnInsert()` — full insert algorithm
- `src/diskann.c` lines 780-837 - `diskAnnInsertShadowRow()` — shadow table insertion
- `src/diskann_node.h` - Node binary format, distance functions, layout helpers
- `src/diskann_search.c` - `diskann_search_internal()`, search context (from knn-search TPP)
- `src/diskann_blob.h` - BlobSpot API (writable mode for insert)
- `src/diskann_internal.h` - DiskAnnIndex struct (pruning_alpha, insert_list_size)

## Description

- **Problem:** `diskann_insert()` is currently a stub returning `DISKANN_ERROR`. The
  insert algorithm in `diskann.c` is coupled to libSQL types (VectorInRow, VectorPair,
  Vector) and uses libSQL-internal APIs (`sqlite3MPrintf`, `sqlite3DbFree`).
- **Constraints:** Must use standalone types from `20250209-shared-graph-types.md`
  (DiskAnnNode, node*bin*\* functions, distance functions). Must use search infrastructure
  from `20250209-knn-search.md` (`diskann_search_internal()`, search context). Float32-only
  eliminates VectorPair entirely. Must wrap multi-row mutations in SAVEPOINT.
- **Success Criteria:**
  - `diskann_insert()` builds correct graph structure on small datasets
  - Inserted vectors are findable by `diskann_search()`
  - Edge lists respect max_neighbors limit
  - Pruning reduces dominated edges correctly
  - First insertion (empty index) succeeds with no edges
  - Duplicate ID insertion fails with appropriate error
  - ASan + Valgrind clean

## Tribal Knowledge

**The insert algorithm has two phases after finding neighbors:**

Phase 1 — Add visited nodes as edges to the NEW node:

- For each visited node, call `replace_edge_idx()` on the new node's BLOB
- This decides: append (if room), replace (if new edge is better), or skip (if pruned)
- After each addition, `prune_edges()` removes edges dominated by the new one
- At the end of Phase 1, the new node has edges pointing to nearby existing nodes

Phase 2 — Add the NEW node as an edge to visited nodes:

- For each visited node, call `replace_edge_idx()` on the visited node's BLOB
- Same replace/append/skip logic, but operating on existing nodes
- After each addition, `prune_edges()` on the visited node's BLOB
- Flush each visited node's BLOB immediately (can't batch — BlobSpot reuse)
- At the end of Phase 2, existing nodes have edges pointing back to the new node

**Edge replacement algorithm (`replace_edge_idx`):**

```
For each existing edge E (reverse order):
  If E.rowid == new_rowid: return E's index (replace zombie/duplicate)
  Compute: edge_to_new = distance(E, new_edge)
  If dist(node, new) > alpha * edge_to_new: return -1 (new edge is dominated, skip)
  If dist(node, new) < dist(node, E): mark E as potential replacement
If room (nEdges < maxEdges): return nEdges (append)
Else: return best replacement index (or -1)
```

**Edge pruning algorithm (`prune_edges`):**
After inserting edge at position `iInserted`:

```
For each edge E (not the new one):
  hint_to_edge = distance(new_edge, E)
  If dist(node, E) > alpha * hint_to_edge: delete E (dominated by new edge)
```

This maintains graph diversity — prevents redundant edges to clustered nodes.

**The `pruning_alpha` parameter** (default 1.2, stored ×1000 in metadata):

- alpha = 1.0: aggressive pruning (sparse graph, fast inserts, lower recall)
- alpha = 1.2: balanced (default, good recall)
- alpha > 1.5: minimal pruning (dense graph, slow inserts, highest recall)

**VectorPair elimination for float32-only:**
The original code uses `VectorPair` to handle separate node/edge vector types
(compression). With float32-only, `nNodeVectorSize == nEdgeVectorSize`, so:

- `initVectorPair()` → no-op (nothing to allocate)
- `loadVectorPair(pair, vec)` → just use the `float*` directly
- `deinitVectorPair()` → no-op (nothing to free)
- `replace_edge_idx()` and `prune_edges()` take `const float*` instead of `VectorPair*`
- The `pNode != pEdge` check in `diskAnnSearchInternal` (line 1366) is always false

**V1 format branches eliminated:**
`diskAnnReplaceEdgeIdx` has `if (nFormatVersion == VECTOR_FORMAT_V1)` which recomputes
distance from scratch (V1 doesn't store distances). We only support V3 where distances
are stored in edge metadata. These branches are removed entirely.

**Shadow row insertion simplified:**

- libSQL: dynamic multi-key INSERT with column name rendering
- Ours: `INSERT INTO "{db}".{idx}_shadow (id, data) VALUES (?, zeroblob(?))`
- The user provides `id` directly. Since `id INTEGER PRIMARY KEY` aliases SQLite's
  rowid, we know the rowid without `RETURNING`. If the id already exists, SQLite
  returns `SQLITE_CONSTRAINT` → map to `DISKANN_ERROR_EXISTS`.

**First insertion special case:**
When the index is empty, `diskann_select_random_shadow_row()` returns no row.
Set `first = 1`, skip the search phase, insert the row, init the BLOB, and return.
The first node has zero edges. The next insert will find it as the starting point.

**SAVEPOINT scope:**
Insert modifies N+1 BLOBs (N neighbor nodes + 1 new node) plus the INSERT statement.
Must wrap in SAVEPOINT. Search runs before the INSERT (to avoid zombie edge confusion
with the new rowid), so search is outside the savepoint — it's read-only anyway.

**Search runs BEFORE shadow row insertion:**
The original code explicitly selects the random start node and runs search before
inserting the new row. Comment from libSQL: "search is made before insertion in order
to simplify life with 'zombie' edges which can have same IDs as new inserted row."
This ordering is important — don't reorder.

**WRITABLE blob mode for insert:**
Search during insert uses `is_writable=1`. This means `diskann_search_internal()`
creates a separate BlobSpot per visited node (instead of reusing one). These BlobSpots
remain live in the visited list so Phase 2 can write edges back to them. The search
context deinit frees them all.

## Solutions

### Option 1: New `src/diskann_insert.c` file ⭐ CHOSEN

**Pros:** Clean separation (like search.c), helpers are insert-specific
**Cons:** Another source file
**Status:** Chosen — `replace_edge_idx` and `prune_edges` are ~130 LOC of insert-only logic

### Option 2: Add to `diskann_api.c`

**Pros:** Fewer files
**Cons:** diskann_api.c already has lifecycle + drop/clear code, would grow too large
**Status:** Rejected

## Tasks

- [ ] Study `diskAnnInsert()` algorithm (lines 1493-1623) in detail
- [ ] Study `diskAnnReplaceEdgeIdx()` (lines 1176-1224) — edge replacement decision
- [ ] Study `diskAnnPruneEdges()` (lines 1229-1280) — dominated edge removal
- [ ] Implement `diskann_insert_shadow_row()` in `src/diskann_insert.c`:
  - `INSERT INTO "{db}".{idx}_shadow (id, data) VALUES (?, zeroblob(?))`
  - Map `SQLITE_CONSTRAINT` → `DISKANN_ERROR_EXISTS`
  - Single-key (no multi-key column rendering)
- [ ] Implement `replace_edge_idx()` (static helper):
  - Takes `const DiskAnnIndex*`, `BlobSpot*` (node), `uint64_t new_rowid`,
    `const float* new_vector`, `float* out_distance`
  - Uses `node_bin_edges()`, `node_bin_edge()`, `node_bin_vector()`, `diskann_distance()`
  - Returns edge index to replace, nEdges to append, or -1 to skip
  - No V1 format branches (V3 only, distances always stored)
  - Uses `idx->pruning_alpha` for angle-based pruning check
- [ ] Implement `prune_edges()` (static helper):
  - Takes `const DiskAnnIndex*`, `BlobSpot*` (node), `int iInserted`
  - Iterates edges, deletes those dominated by newly inserted edge
  - Uses `node_bin_edge()`, `node_bin_delete_edge()`, `diskann_distance()`
  - No V1 branches
- [ ] Implement `diskann_insert()` in `src/diskann_insert.c`:
  - Validate inputs (NULL idx, NULL vector, dimension mismatch)
  - Select random start node via `diskann_select_random_shadow_row()`
  - If not first: run `diskann_search_internal()` with WRITABLE mode, insertL budget
  - Begin SAVEPOINT
  - Insert shadow row (zeroblob)
  - Create BlobSpot for new row, `node_bin_init()`
  - If first: flush new blob, release savepoint, return
  - Phase 1: for each visited node, `replace_edge_idx()` on new node + `prune_edges()`
  - Phase 2: for each visited node, `replace_edge_idx()` on visited node +
    `prune_edges()` + `blob_spot_flush()`
  - Flush new node's blob
  - Release SAVEPOINT (rollback on any error)
  - Free search context
- [ ] Remove `diskann_insert()` stub from `src/diskann_api.c`
- [ ] Write tests in `tests/c/test_insert.c`:
  - Insert with NULL index → `DISKANN_ERROR_INVALID`
  - Insert with NULL vector → `DISKANN_ERROR_INVALID`
  - Insert with wrong dimensions → `DISKANN_ERROR_DIMENSION`
  - Insert first vector into empty index → success, 1 row in shadow table
  - Insert second vector → both have edges to each other
  - Insert 10 vectors → all findable by `diskann_search()`
  - Insert duplicate ID → `DISKANN_ERROR_EXISTS`
  - Insert 100 random vectors → recall > 95% vs brute force
  - Edge count respects `node_edges_max_count()` limit
- [x] Wire `diskann_insert.c` into Makefile SOURCES and test_runner.c
- [x] Remove `diskann_insert()` stub from `src/diskann_api.c`

**Verification:**

```bash
make test      # 122 tests, 0 failures
make asan      # No memory errors
make valgrind  # No leaks, 0 errors
```

## Session Notes (2025-02-09, Implementation)

**SAVEPOINT must precede search.** The original libSQL code doesn't use SAVEPOINTs
(relies on virtual table transaction handling). Our standalone API wraps in SAVEPOINT
for atomicity. Key discovery: the SAVEPOINT must be started BEFORE `search_internal`
(not after), because writable blob handles opened during search cause `SQLITE_BUSY`
if a SAVEPOINT is started while they're open.

**blob_spot_reload required before node_bin_init.** `blob_spot_create` opens a handle
but sets `is_initialized = 0`. `node_bin_init` writes to the buffer but doesn't
set the flag. `blob_spot_flush` requires `is_initialized = 1`. Solution: call
`blob_spot_reload` between `blob_spot_create` and `node_bin_init` to read the
zeroblob and set the flag.

**11 tests written:** validation (3), first vector (1), two vectors bidirectional (1),
duplicate ID (1), 10-vector searchable (1), edge count limit (1), recall (1),
insert-delete-search integration (1), cosine metric (1).

**All 8 public API functions now implemented.** 122 tests total, ASan + Valgrind clean.

## Notes

**Blocked by:** ~~`20250209-knn-search.md`~~ ✅ Complete

**NOT blocked by:** `20250209-vector-delete.md` — insert and delete are independent.

**After this TPP completes:** All 8 public API functions are implemented. The
`src/diskann.c` original libSQL file can be archived or removed. Integration tests
(create → insert → search → delete full workflow) should be written as a separate TPP.
