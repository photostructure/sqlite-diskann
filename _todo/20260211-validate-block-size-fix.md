# Validate Block Size Fix & Benchmark Results

## Summary

**Block size fix IMPLEMENTED.** Need to validate recall improves from 0-1% to 85-95% on 100k vectors.

**Root cause:** 4KB blocks → 2-3 max edges/node → graph fragments at 100k scale.
**Fix:** Auto-calculate block size (256D/32-edges = 40KB) to support proper connectivity.

**Goal:** Verify 85-95% recall @ 100k vectors with new block size.

## Current Phase

- [x] Root Cause Investigation (block size identified)
- [x] Block Size Fix Implementation (Phase 1 complete)
- [ ] **Benchmark Validation (IN PROGRESS)**
- [ ] Documentation & Cleanup
- [ ] Final Review

**Status:** Phase 1 code complete, committed. Ready to validate with benchmarks.

## Required Reading

- `_done/20260210-diskann-recall-fix-investigation.md` - Full investigation history
- `src/diskann_api.c` - Block size calculation implementation
- `MEMORY.md` - Project state and key discoveries

## What Was Fixed

**Commit:** `fix(diskann_api): auto-calculate block size for graph connectivity`

**Changes:**

1. Added `calculate_block_size()` - computes minimum block size based on dimensions × max_neighbors
2. Auto-calculate when config->block_size == 0 (new default)
3. Validate user-provided block_size >= minimum required
4. Added DISKANN_ERROR_VERSION (-8) for format mismatch
5. Store format_version=2 in metadata
6. Version checking on index open (backward compat during dev)

**Block sizes (with 10% margin):**

- 64D/32-edges: 12KB (was 4KB)
- 128D/32-edges: 20KB (was 4KB)
- 256D/32-edges: **40KB** (was 4KB) ← benchmark uses this
- 512D/32-edges: 76KB (was 4KB)
- 768D/32-edges: 112KB (was 4KB)

## Tribal Knowledge

### Why 4KB Blocks Failed at Scale

**Math for 256D vectors:**

```
Node overhead = 16 (metadata) + 1024 (vector) = 1040 bytes
Edge overhead = 1024 (vector) + 16 (metadata) = 1040 bytes

4KB blocks:
  Space for edges = 4096 - 1040 = 3056 bytes
  Max edges = 3056 / 1040 = 2.9 → **2-3 edges per node**

40KB blocks (new):
  Space for edges = 40960 - 1040 = 39,920 bytes
  Max edges = 39,920 / 1040 = 38.4 → **35+ edges per node**
```

**Why 10k worked but 100k failed:**

- 10k vectors: Dense enough to stay connected despite 2-3 edges/node → 97% recall
- 100k vectors: Fragments into isolated components → random start lands in wrong component → 0-1% recall

**All previous parameter fixes were correct but insufficient:**

- searchListSize=500 ✅ (QPS dropped 45%, proving it works)
- pruning_alpha=1.4 ✅
- MIN_DEGREE=8 ✅
- **But max 2-3 edges/node made graph construction impossible**

### libSQL Comparison

libSQL uses 65KB blocks → ~125 max edges/node → graph stays connected at scale.

## Tasks

### Phase 2: Benchmark Validation (2-3 hours)

- [ ] **Rebuild benchmark indices with new block size**

  ```bash
  cd benchmarks
  rm -rf datasets/synthetic/*.db  # Clear old 4KB indices
  npm run prepare  # Rebuild with 40KB blocks
  ```

- [ ] **Run quick benchmark (10k vectors)**

  ```bash
  npm run bench:quick
  ```

  Expected: Should maintain 95-99% recall (was already good at 10k)

- [ ] **Run standard benchmark (100k vectors)**

  ```bash
  npm run bench:standard  # Takes ~20 minutes
  ```

  **CRITICAL SUCCESS METRIC:** Recall improves from 0-1% to 85-95%

- [ ] **Compare results**
  - Before: 0.0-1.0% recall @ k=10-100
  - After: **85-95% recall @ k=10-100** (target)
  - QPS: Should be reasonable (100-500 QPS acceptable)
  - Build time: Should be < 5 minutes for 100k

- [ ] **Document findings**
  - Update this TPP with actual recall achieved
  - Update MEMORY.md with success confirmation
  - If recall < 85%, investigate further (may need multi-start)

### Phase 3: Documentation & Cleanup (1-2 hours)

- [ ] Update README.md with block size auto-calculation
- [ ] Document breaking change (old indices must be rebuilt)
- [ ] Add migration guide for users
- [ ] Update CLAUDE.md if needed
- [ ] Clean up backward compat code (format version check)

### Phase 4: Final Review

- [ ] All 175 tests pass (64 metadata failures are pre-existing, tracked separately)
- [ ] ASan clean: `make clean && make asan`
- [ ] Valgrind clean: `make clean && make valgrind`
- [ ] TypeScript tests pass: `npm test`
- [ ] Standard benchmark: 85-95% recall @ 100k ✅

## Optional: Priority 2 - Multiple Random Starts (4-6 hours)

**If recall is 70-85% instead of 85-95%, add robustness:**

Use 3-5 random entry points, merge results. Provides robustness against minor fragmentation.

**Implementation:**

- Add `diskann_search_multi_start()` to `src/diskann_search.c`
- Adaptive defaults: 10k=1 start, 100k=3 starts, 1M+=5 starts
- Expose via TypeScript `SearchOptions.numRandomStarts`

**Trade-off:** 3-5x slower but correct vs broken.

## Optional: Priority 3 - Graph Health Diagnostics (8-12 hours)

**For validation and future debugging:**

Add BFS-based connected components counting.

**API:**

```c
typedef struct {
  uint64_t total_nodes;
  uint64_t connected_components;
  double connectivity_ratio;  // largest_component / total_nodes
  uint32_t min_degree, max_degree;
  double avg_degree;
} GraphStats;

int diskann_compute_stats(DiskAnnIndex *idx, GraphStats *stats);
```

**Use cases:**

- Verify block size fix worked (components=1, connectivity≥0.95)
- Debug future recall issues
- Monitor graph health in production

## Expected Outcomes

### Before (4KB blocks)

- 10k vectors: 97% recall ✅
- 100k vectors: 0-1% recall ❌
- Max edges: 2-3 per node
- Graph: Fragmented at scale

### After (40KB blocks)

- 10k vectors: 97% recall ✅ (maintained)
- 100k vectors: **85-95% recall** ✅ (target)
- Max edges: 35+ per node
- Graph: Well-connected

### If 70-85% recall (Phase 2 + Priority 2)

- Add multi-start: 3-5 random entry points
- Expected: 90-97% recall
- Cost: 3-5x slower (acceptable for correctness)

## Verification Commands

```bash
# Rebuild indices with new block size
cd benchmarks && rm -rf datasets/synthetic/*.db && npm run prepare

# Quick check (10k)
npm run bench:quick

# Full validation (100k) - CRITICAL TEST
npm run bench:standard

# Check for memory leaks
cd .. && make clean && make valgrind

# All tests
make test
npm test
```

## Session Notes

### Session 2026-02-11: Block Size Fix Implementation

**Investigation (3 hours):**

- Launched 6 parallel agents (3 Explore + 3 Plan)
- Agent 2 (libSQL comparison) found the smoking gun: 65KB blocks vs 4KB blocks
- Root cause confirmed: Block size limitation prevents proper graph connectivity

**Implementation (2 hours):**

- Added `calculate_block_size()` with formula: node + (margins × 1.1) × edges
- Auto-calculate when block_size=0 (new default)
- Validation: user-provided >= minimum
- Format version=2 in metadata
- Backward compat for testing (allow version 1 indices to open)

**Testing:**

- ✅ Code compiles cleanly
- ✅ Calculation verified: 256D/32-edges = 40KB
- ✅ Core tests pass
- ✅ Committed with comprehensive message

**Validation Session (Current):**

**Critical Bug Found:** Virtual table xCreate hardcoded block_size=4096, causing index creation to fail.

- **Symptom:** Quick benchmark failed with "rc=-4" (DISKANN_ERROR_INVALID)
- **Root cause:** diskann_vtab.c line 359 set `config.block_size = 4096`
- **Fix:** Changed to `config.block_size = 0` (auto-calculate)
- **Impact:** vtab now auto-calculates correct block size based on dimensions/max_neighbors

**Quick Benchmark (10k, 64D) - SUCCESS:**

- ✅ Build time: 93.3s
- ✅ Index size: 200.2 MB (10k × 20KB blocks)
- ✅ Recall: **100.0%** @ k=10
- ✅ QPS: 609 (slower than brute force on small dataset, expected)
- **Conclusion:** Block size fix works correctly on small scale

**Standard Benchmark (100k, 256D) - IN PROGRESS:**

- Running now to validate recall improves from 0-1% to 85-95%
- Expected: 40KB blocks × 100k vectors = 4GB index
- Target: 85-95% recall @ k=10-100

**Next session should:**

1. ✅ ~~Run quick benchmark~~ - COMPLETE (100% recall)
2. 🔄 Run standard benchmark - IN PROGRESS
3. Document results and update README
4. Optional: Add multi-start if recall < 85%

**Known Issues:**

- 64 pre-existing metadata test failures (unrelated to block size fix)
- Tracked separately, doesn't block recall validation

## Critical Files

- `src/diskann_api.c` - Block size calculation and validation
- `src/diskann.h` - DISKANN_ERROR_VERSION (-8)
- `benchmarks/` - Validation tests
- `_done/20260210-diskann-recall-fix-investigation.md` - Full investigation history

## Time Estimate

| Phase                   | Tasks                   | Hours         |
| ----------------------- | ----------------------- | ------------- |
| 2. Benchmark Validation | Rebuild + run + verify  | 2-3           |
| 3. Documentation        | README, migration guide | 1-2           |
| 4. Final Review         | Tests, ASan, Valgrind   | 1             |
| **Total Required**      |                         | **4-6 hours** |
| Optional: Multi-start   | Robustness if needed    | 4-6           |
| Optional: Diagnostics   | Graph health API        | 8-12          |

**Minimum to close:** Phases 2-4 (4-6 hours) if recall ≥ 85%.
