# Vector Delete Implementation

## Summary

Extract and implement `diskann_delete()` — remove a vector from the DiskANN graph index.
The delete algorithm reads the target node's edges, removes back-edges from all neighbors
pointing to the deleted node, then deletes the shadow table row. Uses a conservative
"no-repair" strategy where the graph gradually becomes sparser with deletions.

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
- `src/diskann.c` lines 1626-1703 - `diskAnnDelete()` implementation
- `src/diskann.c` lines 668-721 - `diskAnnGetShadowRowid()` (rowid lookup)
- `src/diskann.c` lines 842-875 - `diskAnnDeleteShadowRow()` (row removal)
- `src/diskann.c` lines 351-397 - `nodeBinEdges/Edge/EdgeFindIdx` (edge reading)
- `src/diskann.c` lines 426-448 - `nodeBinDeleteEdge()` (edge removal)
- `src/diskann.h` - Public API (diskann_delete signature)
- `src/diskann_blob.h` - BlobSpot API (read/write node BLOBs)
- `src/diskann_internal.h` - DiskAnnIndex struct

## Description

- **Problem:** `diskann_delete()` is currently a stub returning `DISKANN_ERROR`. The
  delete algorithm in `diskann.c` is coupled to libSQL types (VectorInRow, VectorIdxKey).
- **Constraints:** Must use standalone types from `20250209-shared-graph-types.md`.
  Our API is simpler than libSQL's — we delete by rowid directly (no multi-key lookup).
  Must use existing BlobSpot layer. Must wrap multi-row mutations in SAVEPOINT.
- **Success Criteria:**
  - `diskann_delete()` removes vector and cleans up neighbor edges
  - Deleting non-existent ID returns `DISKANN_ERROR_NOTFOUND`
  - Shadow table row count decreases by 1 after delete
  - Neighbor nodes' edge lists no longer reference deleted node
  - Search still works after deletion (handles zombie edges gracefully)
  - ASan + Valgrind clean

## Tribal Knowledge

**The delete algorithm is conservative — no graph repair:**

1. Load target node's BLOB (get its edge list)
2. For each edge (neighbor) in the target node:
   a. Load neighbor's BLOB
   b. Find back-edge pointing to target node (`nodeBinEdgeFindIdx`)
   c. If found: remove it (`nodeBinDeleteEdge` — swap-with-last), flush BLOB
3. Delete target row from shadow table
4. Done — no reconnection of orphaned paths

**Why no graph repair?** Reconnecting edges after delete is expensive (requires running
search to find replacement edges) and the conservative approach works well in practice:

- Zombie edges (edges pointing to deleted nodes) are tolerated
- `diskAnnSearchInternal` handles `DISKANN_ERROR_NOTFOUND` by skipping
- Graph quality degrades gradually, not catastrophically
- For heavy-delete workloads, periodic rebuild is recommended

**SAVEPOINT is important:** Delete modifies N+1 rows (N neighbors + 1 target row).
If it fails partway through, some neighbors have had their back-edge removed but the
target node still exists — leaving the graph inconsistent. Wrap in SAVEPOINT.

**Our API simplification vs libSQL:**

- libSQL: `diskAnnDelete(pIndex, VectorInRow *pInRow, ...)` with multi-key lookup
- Ours: `diskann_delete(DiskAnnIndex *idx, int64_t id)` — delete by rowid directly
- This eliminates `diskAnnGetShadowRowid()` entirely — we already have the rowid
- Just need: check row exists → load BLOB → clean neighbors → delete row

**Edge deletion uses swap-with-last:** `nodeBinDeleteEdge()` doesn't shift elements.
It copies the last edge into the deleted slot and decrements the count. O(1) but
changes edge ordering. This is fine — edge order doesn't matter for correctness.

**`nodeBinEdgeFindIdx` is linear scan:** O(M) where M = edges per node. The libSQL
code has a TODO comment about making this a binary search, but M is typically 32-64
so linear is fine.

**Flush after each neighbor modification:** Each neighbor BLOB must be flushed
(`blob_spot_flush`) immediately after removing the back-edge. Can't batch because
BlobSpot reuse via `blob_spot_reload()` overwrites the buffer.

## Solutions

### Option 1: Implement directly in `diskann_api.c` ⭐ CHOSEN

**Pros:** Delete is ~80 lines of logic, doesn't warrant its own file. All public API
functions live in diskann_api.c already.
**Cons:** diskann_api.c grows slightly
**Status:** Chosen — right-sized for the existing file

### Option 2: Separate `diskann_delete.c`

**Pros:** Isolation
**Cons:** Overkill for ~80 lines of new code
**Status:** Rejected

## Tasks

- [x] Study `diskAnnDelete()` algorithm (lines 1626-1703)
- [x] Study `nodeBinDeleteEdge()` (lines 426-448) — swap-with-last pattern
- [x] **Bug found in original libSQL code (line 1676):** `nodeBinEdgeFindIdx(pIndex,
pEdgeBlob, edgeRowid)` searches for the NEIGHBOR's own rowid instead of the deleted
      node's rowid (`nodeRowid`). Back-edges were never actually cleaned up. Fixed in our
      extraction.
- [x] Write tests in `tests/c/test_delete.c` (TDD — tests first):
  - Delete NULL index → `DISKANN_ERROR_INVALID`
  - Delete from empty index → `DISKANN_ERROR_NOTFOUND`
  - Delete non-existent ID → `DISKANN_ERROR_NOTFOUND`
  - Delete single node (no edges) → row removed, table still exists
  - Delete node with edges → back-edges cleaned from neighbors
  - Delete last node → index empty but functional
  - Double delete → second returns `DISKANN_ERROR_NOTFOUND`
  - Zombie edge (neighbor already deleted) → succeeds
- [x] Wire into test_runner.c
- [x] Implement delete in `src/diskann_api.c`:
  - Validate inputs (NULL idx)
  - Begin SAVEPOINT
  - Load target node BLOB via `blob_spot_create()` (read-only)
  - If not found → rollback, return `DISKANN_ERROR_NOTFOUND`
  - Read edge count with `node_bin_edges()`
  - If edges > 0, create writable BlobSpot for neighbor editing
  - For each edge:
    - Read neighbor rowid with `node_bin_edge()`
    - Load neighbor BLOB via `blob_spot_reload()`
    - If DISKANN_ROW_NOT_FOUND → continue (zombie edge)
    - Find back-edge with `node_bin_edge_find_idx(idx, edge_blob, id)` ← FIXED
    - If found: `node_bin_delete_edge()` + `blob_spot_flush()`
  - Delete shadow table row: `DELETE FROM "%w".%s WHERE id = ?`
  - Verify `sqlite3_changes() == 1`
  - Release SAVEPOINT
- [x] Verify: `make test` (8/8 delete tests pass), `make asan` (clean), `make valgrind` (clean)

**Verification:**

```bash
make test      # All tests pass
make asan      # No memory errors
make valgrind  # No leaks
```

## Notes

**Blocked by:** `20250209-shared-graph-types.md` (needs nodeBinEdges, nodeBinEdge,
nodeBinEdgeFindIdx, nodeBinDeleteEdge)

**NOT blocked by:** `20250209-knn-search.md` — delete doesn't need search.

**Test data construction:** Like the search TPP, tests need to manually construct node
BLOBs with known edge structures using `nodeBin*` helpers. Write test helpers that
build small graphs (e.g., 3 nodes with bidirectional edges) directly in the shadow table.

**Future consideration:** After both search and insert are implemented, add an
integration test: insert 100 vectors → delete 10 → search still returns correct results
from remaining 90. This validates the zombie-edge handling in search.
