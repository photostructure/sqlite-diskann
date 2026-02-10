# Shared Graph Types & Node Binary Format

## Summary

Port DiskANN's internal types (DiskAnnNode, DiskAnnSearchCtx) and shared helper functions from the coupled `src/diskann.c` into standalone modules. This is the foundation that unblocks search, insert, and delete extraction.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- `src/diskann.c` lines 80-120 - Type definitions (DiskAnnNode, DiskAnnSearchCtx, VectorPair)
- `src/diskann.c` lines 126-166 - LE serialization helpers (readLE*/writeLE*)
- `src/diskann.c` lines 297-470 - Node binary format (layout helpers + nodeBin* functions)
- `src/diskann.c` lines 881-952 - Vector pair helpers, distance calculation, buffer management
- `src/diskann.c` lines 958-968 - diskAnnVectorDistance dispatcher (L2 + cosine only)
- `src/diskann.c` lines 971-988 - diskAnnNodeAlloc/Free
- `src/diskann.c` lines 1706-1775 - diskAnnOpenIndex (shows all DiskAnnIndex fields)
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
  - Distance calculations match reference values for L2 and cosine
  - LE serialization passes tests against known byte sequences
  - All existing tests still pass
  - ASan + Valgrind clean

## Tribal Knowledge

**Key decisions:**
- **Format version:** V3 only (latest). Drop V1/V2 compatibility entirely.
  - V3 node metadata = 16 bytes (`u64 rowid` + `u64` where only low 16 bits are edge count)
  - V2 node metadata = 10 bytes — we do NOT support this.
- **Float32-only:** No edge compression. `nNodeVectorSize == nEdgeVectorSize` always.
  This eliminates VectorPair entirely (no separate node/edge vector types).
- **No dot product:** libSQL only implements L2 and cosine distance. Port those two only.
  Dot product would be new functionality — defer to a separate TPP if needed.

**Node BLOB layout (V3 format, float32-only):**
```
[0-7]    Node rowid (uint64_t LE)
[8-15]   Edge count in low 16 bits (uint64_t LE, upper 48 bits zero/reserved)
[16...]  Node vector (dims × sizeof(float) = nNodeVectorSize bytes)
[...]    Edge vectors (maxEdges × nEdgeVectorSize bytes — ALL slots, not just used)
[...]    Edge metadata (maxEdges × 16 bytes each)
```

**Edge metadata layout (16 bytes per edge, NOT 12):**
```
[0-3]   Unused/padding (4 bytes)
[4-7]   Distance as float, stored as uint32_t LE (4 bytes)
[8-15]  Target rowid (uint64_t LE, 8 bytes)
```

This was verified from `nodeBinReplaceEdge()` (line 419-420):
- Distance written at `edgeMetaOffset + sizeof(u32)` (offset 4)
- Rowid written at `edgeMetaOffset + sizeof(u64)` (offset 8)
And `edgeMetadataSize()` returns `sizeof(u64) + sizeof(u64)` = 16.

**Critical: `nodeEdgesMetadataOffset`** — Edge metadata isn't contiguous with edge vectors.
The metadata section starts after ALL edge vector slots (even unused ones). Get this
offset wrong and everything corrupts silently. See `diskann.c` lines 327-333.

**Max edges per node** depends on block_size, vector dimensions:
- Formula: `(block_size - nodeOverhead) / edgeOverhead`
- nodeOverhead = nodeMetadataSize(16) + nNodeVectorSize
- edgeOverhead = nEdgeVectorSize + edgeMetadataSize(16)
- 768D float32, 4096 block → 0 edges (too small!)
- Realistic configs need larger block sizes

**libSQL type replacements:**
- `u8` → `uint8_t`
- `u16` → `uint16_t`
- `u32` → `uint32_t`
- `u64` → `uint64_t`
- `Vector*` → `float*` (float32-only, zero-copy pointer into BLOB buffer)
- `vectorSerializeToBlob(vec, dest, size)` → `memcpy(dest, data, dims * sizeof(float))`
- `vectorInitStatic(vec, type, dims, data)` → just return `(float*)data` (pointer into buffer)

**DiskAnnIndex needs additional fields for node layout** (currently missing from
`diskann_internal.h`):
- `nNodeVectorSize` (uint32_t) — `dims * sizeof(float)` for float32
- `nEdgeVectorSize` (uint32_t) — same as nNodeVectorSize for float32-only
- `pruning_alpha` (double) — edge pruning threshold, default 1.2
These are derived from existing config fields at open time.

**VectorPair is eliminated.** For float32-only, node and edge vectors are the same type.
`DiskAnnSearchCtx.query` becomes `float *query_data` + dims (from index config).

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

- [x] **Research & validate TPP** — verify all claims against source code
- [x] **Extend DiskAnnIndex** in `src/diskann_internal.h`:
  - Add `nNodeVectorSize` (uint32_t) — computed as `dims * sizeof(float)`
  - Add `nEdgeVectorSize` (uint32_t) — same as nNodeVectorSize for float32
  - Add `pruning_alpha` (double) — edge pruning threshold
  - Update `diskann_open_index()` to compute derived fields
  - Update `diskann_create_index()` to store pruning_alpha in metadata
- [x] Define standalone types in `src/diskann_node.h`:
  - `DiskAnnNode` (uint64_t rowid, int visited, DiskAnnNode *next, BlobSpot *blob_spot)
  - Layout calculation helpers (NODE_METADATA_SIZE, EDGE_METADATA_SIZE constants,
    node_edges_max_count, node_edges_metadata_offset)
  - LE serialization: `read_le16/32/64`, `write_le16/32/64` (inline functions)
  - **Note:** DiskAnnSearchCtx deferred — will be defined when implementing search
- [x] Implement node binary functions in `src/diskann_node.c`:
  - `node_bin_init()` — Initialize node BLOB with rowid + vector (memcpy)
  - `node_bin_vector()` — Return const float* pointer into BLOB buffer (zero-copy)
  - `node_bin_edges()` — Read edge count from BLOB
  - `node_bin_edge()` — Read edge at index (rowid, distance, vector pointer)
  - `node_bin_edge_find_idx()` — Find edge by target rowid
  - `node_bin_replace_edge()` — Write/append edge (memcpy for vector data)
  - `node_bin_delete_edge()` — Remove edge (swap with last)
  - `node_bin_prune_edges()` — Truncate edge list
- [x] Implement distance calculation in `src/diskann_node.c`:
  - `diskann_distance_l2()` — L2 (Euclidean squared) distance
  - `diskann_distance_cosine()` — Cosine distance
  - `diskann_distance()` — Dispatcher by metric type
- [x] Implement buffer management in `src/diskann_node.c`:
  - `buffer_insert()` / `buffer_delete()` — Generic sorted array operations
  - `distance_buffer_insert_idx()` — Find insertion position for distance
- [x] Implement node alloc/free in `src/diskann_node.c`:
  - `diskann_node_alloc()` / `diskann_node_free()`
- [x] Write tests in `tests/c/test_node_binary.c` (39 new tests):
  - LE serialization roundtrip tests (5)
  - Layout calculation tests (4)
  - Node BLOB init + read back vector, edge CRUD (9)
  - Distance L2 + cosine with known values (8)
  - Buffer insert/delete operations (10)
  - Node alloc/free (2)
  - DiskAnnIndex derived fields from open_index (1)
- [x] Wire into Makefile and test_runner.c

**Verification:**
```bash
make test      # All tests pass (existing + new)
make asan      # No memory errors
make valgrind  # No leaks
```

## Notes

**Blocks:** `20250209-knn-search.md`, `20250209-vector-delete.md`, future insert TPP
