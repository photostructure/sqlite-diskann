# Shared Graph Types & Node Binary Format

## Summary

Port DiskANN's internal types (DiskAnnNode, DiskAnnSearchCtx, VectorPair) and shared helper functions from the coupled `src/diskann.c` into standalone modules. This is the foundation that unblocks search, insert, and delete extraction.

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
- `src/diskann.c` lines 80-170 - Type definitions (DiskAnnNode, DiskAnnSearchCtx, VectorPair)
- `src/diskann.c` lines 126-166 - LE serialization helpers (readLE*/writeLE*)
- `src/diskann.c` lines 295-470 - Node binary format (nodeBin* functions)
- `src/diskann.c` lines 881-970 - Vector pair helpers, distance calculation, node alloc
- `src/diskann.c` lines 911-952 - Buffer management (bufferInsert/bufferDelete)
- `src/diskann_internal.h` - Existing internal structs
- `src/diskann_blob.h` - BlobSpot (already extracted, used by DiskAnnNode)
- `_research/IMPLEMENTATION-DESIGN.md` - Decoupling strategy

## Description

- **Problem:** The core algorithm functions (search, insert, delete) all depend on shared
  types and helper functions that are currently trapped inside the coupled `src/diskann.c`.
  No algorithm extraction can proceed without these foundations.
- **Constraints:** Must replace libSQL-specific types (`Vector`, `u64`, etc.) with
  standalone equivalents. Must compile with project's strict warning flags.
  Float32-only for now (no compression/quantization).
- **Success Criteria:**
  - New files `src/diskann_node.h` and `src/diskann_node.c` compile cleanly
  - All nodeBin* functions pass roundtrip tests (write then read back)
  - Distance calculations match reference values for L2, cosine, dot product
  - LE serialization passes cross-platform tests (known byte sequences)
  - All existing 46 tests still pass
  - ASan + Valgrind clean

## Tribal Knowledge

**Node BLOB layout (4096 bytes default):**
```
[0-7]   Node rowid (u64 LE)
[8-9]   Edge count (u16 LE)
[10...]  Node vector (dims × sizeof(float))
[...]   Edge vectors (count × edgeVectorSize)
[...]   Edge metadata (count × 12 bytes: 4b distance + 8b target rowid)
```

**Critical: `nodeEdgesMetadataOffset`** — Edge metadata isn't contiguous with edge vectors.
The metadata section starts after ALL edge vector data. Get this offset wrong and
everything corrupts silently. See `diskann.c` lines 327-333.

**Max edges per node** depends on block_size, vector dimensions, and compression:
- 768D float32, no compression, 4096 block → 0 edges (too small!)
- Realistic configs need larger block sizes or edge compression
- Start with float32-only, meaning block_size must be large enough

**libSQL uses `u64` (unsigned 64-bit), `u16`, etc.** Replace with `uint64_t`, `uint16_t`.

**VectorPair** exists for optional edge compression (store edges at lower precision than
node vectors). For float32-only initial implementation, `pNode == pEdge` always. Still
need the struct for API compatibility, but initVectorPair/loadVectorPair become trivial.

**Distance functions** in libSQL live in a separate file (`vectorIndex.c`). We need to
reimplement L2, cosine, and dot product from scratch — they're simple math, not worth
extracting the libSQL vector infrastructure.

## Solutions

### Option 1: Single `diskann_node.h/.c` module ⭐ CHOSEN
**Pros:** All node-level operations in one place, simple to test
**Cons:** File may grow large
**Status:** Chosen — natural cohesion, everything operates on the same BLOB layout

### Option 2: Split into separate files (node, distance, serialization)
**Pros:** Smaller files
**Cons:** Over-engineering for ~350 LOC total, more headers to manage
**Status:** Rejected — premature splitting

## Tasks

- [ ] Define standalone types in `src/diskann_node.h`:
  - `DiskAnnNode` (replace `u64` → `uint64_t`, `BlobSpot*` from our blob layer)
  - `DiskAnnSearchCtx` (replace `Vector*` with `float*` + dims)
  - Layout calculation helpers (nodeOverhead, edgeMetadataSize, nodeEdgesMaxCount, etc.)
- [ ] Implement LE serialization in `src/diskann_node.c`:
  - `readLE16/32/64`, `writeLE16/32/64` (from diskann.c lines 126-166)
  - Test against known byte sequences
- [ ] Implement node binary functions:
  - `nodeBinInit()` — Initialize node BLOB with rowid + vector
  - `nodeBinVector()` — Read node vector from BLOB
  - `nodeBinEdges()` — Read edge count
  - `nodeBinEdge()` — Read edge at index (rowid, distance, vector)
  - `nodeBinEdgeFindIdx()` — Find edge by target rowid
  - `nodeBinReplaceEdge()` — Write/append edge
  - `nodeBinDeleteEdge()` — Remove edge (swap with last)
  - `nodeBinPruneEdges()` — Truncate edge list
- [ ] Implement distance calculation:
  - `diskann_distance_l2()` — Euclidean distance
  - `diskann_distance_cosine()` — Cosine distance
  - `diskann_distance_dot()` — Dot product distance
  - `diskann_distance()` — Dispatcher by metric type
- [ ] Implement buffer management:
  - `bufferInsert()` / `bufferDelete()` — Generic sorted array operations
  - `distanceBufferInsertIdx()` — Find insertion position for distance
- [ ] Implement node alloc/free:
  - `diskann_node_alloc()` / `diskann_node_free()`
- [ ] Write tests in `tests/c/test_node_binary.c`
- [ ] Wire into Makefile and test_runner.c

**Verification:**
```bash
make test      # All tests pass (existing + new)
make asan      # No memory errors
make valgrind  # No leaks
```

## Notes

**Blocks:** `20250209-knn-search.md`, `20250209-vector-delete.md`, future insert TPP
