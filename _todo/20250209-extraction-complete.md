# Extraction Complete: Archive libSQL Code & Integration Tests

## Summary

Final cleanup after all 8 public API functions are implemented. Archive the original
coupled `src/diskann.c`, remove dead code and stale references, write end-to-end
integration tests covering the full create → insert → search → delete workflow, and
update the parent TPP and project documentation.

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
- `src/diskann.c` - Original libSQL code (should have zero remaining references)
- `src/diskann.h` - Public API (all 8 functions should be implemented)
- `src/diskann_api.c` - Lifecycle functions (create, open, close, drop, clear)
- `src/diskann_search.c` - Search implementation (from knn-search TPP)
- `src/diskann_insert.c` - Insert implementation (from vector-insert TPP)
- `src/diskann_node.h/.c` - Node binary format (from shared-graph-types TPP)
- `src/diskann_blob.h/.c` - BLOB I/O layer
- `Makefile` - Build system (verify diskann.c is not in SOURCES)
- `_todo/20250209-diskann-extraction.md` - Parent TPP with overall task tracking

## Description

- **Problem:** After search, insert, and delete are extracted into standalone modules,
  the original `src/diskann.c` (1789 lines of coupled libSQL code) serves no purpose.
  It still includes `sqliteInt.h` and `vectorIndexInt.h` which don't exist in our tree.
  It should be archived, and we need integration tests proving the extracted code works
  end-to-end.
- **Constraints:** Must verify zero references to `diskann.c` before removing. Must
  ensure all 8 public API functions work together in realistic workflows. Must verify
  test coverage is adequate across all modules.
- **Success Criteria:**
  - `src/diskann.c` moved to `_research/diskann-original.c` (preserved for reference)
  - No source file includes or references the original code
  - Makefile does not reference `diskann.c`
  - Integration tests cover: create → insert N vectors → search → delete → search again
  - All tests pass (unit + integration), ASan + Valgrind clean
  - Parent TPP (`20250209-diskann-extraction.md`) updated to reflect completion
  - All completed TPPs moved to `_done/`

## Tribal Knowledge

**`diskann.c` is NOT compiled today.** The Makefile SOURCES line already excludes it:
```makefile
# Note: diskann.c is the original libSQL code still being extracted - not included yet
SOURCES = $(SRC_DIR)/diskann_api.c $(SRC_DIR)/diskann_blob.c
```
So removing it won't break the build. But verify no `#include` or reference exists
elsewhere.

**Integration tests need realistic data.** Unit tests use hand-crafted 3-4 dimension
vectors. Integration tests should use higher dimensions (e.g., 64D or 128D) with
enough vectors (100-1000) to exercise the graph traversal meaningfully. Use seeded
random vectors for reproducibility.

**Recall measurement:** Insert N random vectors, then query each vector against the
index. Compare k-NN results to brute-force. Recall@10 should be > 95% for a
well-constructed graph. This is the key quality metric.

**Performance sanity check (not a hard gate):** On 1000 vectors, 128D:
- Insert: < 100ms per vector
- Search k=10: < 1ms per query
These are rough sanity checks, not formal benchmarks. If wildly off, something is wrong.

**Delete + search interaction:** After deleting vectors, search should still return
correct results from the remaining set. Zombie edges (pointing to deleted nodes) should
be handled gracefully by search. This is the key integration test for delete.

**Clear + recreate:** After `diskann_clear_index()`, metadata should be preserved but
all vectors gone. Reinserting should work. After `diskann_drop_index()`, everything
is gone and `diskann_open_index()` should fail with `DISKANN_ERROR_NOTFOUND`.

## Solutions

### Option 1: Archive to `_research/` ⭐ CHOSEN
**Pros:** Preserves original for reference, clear signal it's not production code
**Cons:** Slightly cluttered _research directory
**Status:** Chosen — may still need to refer back during debugging

### Option 2: Delete entirely
**Pros:** Clean
**Cons:** Loses the reference implementation; git history exists but is harder to browse
**Status:** Rejected — too aggressive

## Tasks

- [ ] Verify `diskann.c` has zero references in the codebase:
  - Grep for `diskann.c` in Makefile, source files, headers, scripts
  - Grep for `#include "sqliteInt.h"` and `#include "vectorIndexInt.h"` in `src/`
  - Confirm SOURCES in Makefile lists only extracted modules
- [ ] Move `src/diskann.c` → `_research/diskann-original.c`
- [ ] Verify all 8 public API stubs in `diskann_api.c` are replaced or removed:
  - `diskann_insert()` stub → replaced by `diskann_insert.c`
  - `diskann_search()` stub → replaced by `diskann_search.c`
  - `diskann_delete()` stub → replaced by real implementation
- [ ] Write integration tests in `tests/c/test_integration.c`:
  - Full lifecycle: create → open → insert 100 vectors → search → close → reopen → search
  - Delete workflow: insert 50 → delete 10 → search remaining 40 → correct results
  - Clear workflow: insert → clear → verify empty → reinsert → verify findable
  - Drop workflow: create → drop → open fails with NOTFOUND
  - Recall test: insert 500 seeded random 128D vectors, query 50, recall@10 > 95%
  - Error paths: search empty index, insert wrong dimensions, delete nonexistent
- [ ] Update `_todo/20250209-diskann-extraction.md`:
  - Check off remaining Phase 2 tasks
  - Update test count
  - Update "Functions Implemented" to 8/8
  - Mark Phase 2 complete
- [ ] Move completed TPPs to `_done/`:
  - `20250209-shared-graph-types.md`
  - `20250209-knn-search.md`
  - `20250209-vector-delete.md`
  - `20250209-vector-insert.md`
  - `20250209-extraction-complete.md` (this TPP, last to move)

**Verification:**
```bash
make test      # All tests pass (unit + integration)
make asan      # No memory errors
make valgrind  # No leaks
grep -r "diskann\.c" src/ Makefile  # No references
```

## Notes

**Blocked by:** `20250209-knn-search.md`, `20250209-vector-insert.md`,
`20250209-vector-delete.md` (all three must be complete)

**This is the final TPP.** After completion, the DiskANN extraction from libSQL is done.
Remaining work (CI/CD, cross-platform builds, npm packaging, PhotoStructure integration)
are separate projects.
