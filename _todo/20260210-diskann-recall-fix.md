# Fix DiskANN Recall Failure at Scale

## Summary

DiskANN shows 97% recall on 10k vectors but catastrophic 1% recall on 100k vectors. Root causes: (1) TypeScript `searchNearest()` missing `searchListSize` parameter so benchmark config ignored, (2) hardcoded search beam width of 100 too small for large datasets, (3) aggressive edge pruning (alpha=1.2) creates poorly connected graphs at scale.

**Goal:** Achieve 85-95% recall on 100k+ datasets via TDD approach with fast tests.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

**Status:** All 4 fixes implemented, recall test compiling, need to verify metadata tests

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `_todo/20260210-benchmark-framework.md` - Benchmark TPP documenting the issue
- `src/index.ts` - TypeScript API (missing searchListSize parameter)
- `src/diskann_vtab.c` - SQL interface (needs search_list_size constraint)
- `src/diskann_api.c` - Default constants (pruning_alpha)
- `src/diskann_insert.c` - Edge pruning logic (needs min degree enforcement)

## Description

**Problem:** Benchmark framework discovered DiskANN recall degrades catastrophically at scale:

- Small dataset (10k): 97% recall ✅
- Medium dataset (100k): 1% recall ❌

**Root Causes Identified:**

1. **TypeScript API bug**: `searchNearest()` (line 262, `src/index.ts`) doesn't accept `searchListSize` parameter. Benchmark runner passes it as 5th arg (line 94, `diskann-runner.ts`) but it's silently ignored.

2. **Hardcoded beam width**: Falls back to `DEFAULT_SEARCH_LIST_SIZE = 100` (line 24, `diskann_api.c`). For 100k vectors, beam of 100 explores only 0.1% of graph.

3. **Over-pruning**: `DEFAULT_PRUNING_ALPHA = 1.2` (line 27, `diskann_api.c`) too aggressive. Removes edges aggressively, creating poorly connected graphs at scale.

4. **No minimum degree**: Pruning logic (line 113, `diskann_insert.c`) can isolate nodes. Only has `assert(n_edges > 0)` which doesn't prevent the problem.

**Success Criteria:**

- Fast test (<30s) reproduces and validates the issue
- TypeScript API accepts searchListSize parameter
- C implementation supports per-query beam width override
- Recall on 100k dataset improves from 1% to 85-95%
- All existing tests still pass

## Tribal Knowledge

### Why Small Works But Large Fails

- **10k vectors + beam 100** = 1% graph exploration → sufficient for dense small graphs
- **100k vectors + beam 100** = 0.1% graph exploration → insufficient, especially with poor connectivity

### Benchmark Configuration Evidence

- `quick.json`: `searchListSize: [100]` → 97% recall @ 10k ✅
- `standard.json`: `searchListSize: [500]` → 1% recall @ 100k ❌ (parameter ignored!)

The 500 value never reaches the C implementation because TypeScript doesn't pass it through.

### Why Aggressive Pruning Hurts at Scale

With `alpha=1.2`, an edge is removed if `distance_to_edge > 1.2 × distance_to_hint`. At scale:

- Early insertions find many candidates (graph is small)
- Later insertions find fewer candidates (search with limited beam)
- Pruning removes "redundant" edges, reducing connectivity
- Graph fragments into poorly connected components
- Random entry point lands in wrong component → recall crashes

### Minimum Degree Critical

Research shows DiskANN needs `MIN_DEGREE >= 8` to maintain connectivity. Current code only asserts `n_edges > 0` after pruning (line 122, `diskann_insert.c`) - doesn't prevent nodes dropping to 1-2 edges.

### Fast Test Strategy

Use 5000 vectors (not 100k) to reproduce issue:

- Builds in ~10 seconds vs 17 minutes
- Still shows recall degradation (beam 100 vs 200)
- Fast enough for TDD cycle
- Use 64 dimensions (not 256) for speed

## Solutions

### Option 1: TypeScript API + C Constraint (CHOSEN)

**Approach:** Add `searchListSize` to TypeScript API, support as SQL constraint in vtab.

**Pros:**

- Backward compatible (optional parameter)
- Enables per-query tuning
- Follows existing k constraint pattern

**Cons:**

- Requires changes in both TypeScript and C
- SQL constraint parsing complexity

**Status:** CHOSEN - highest impact, follows existing patterns

### Option 2: Adaptive Beam Scaling in C (FUTURE)

**Approach:** Auto-scale beam based on index size (e.g., 100k → 5x beam)

**Pros:**

- Automatic, no user tuning
- Fixes issue without API changes

**Cons:**

- Adds COUNT(\*) query overhead
- Less control for power users
- May over-scale for some workloads

**Status:** DEFERRED - implement after explicit parameter support

### Option 3: Change Defaults Only (INSUFFICIENT)

**Approach:** Just increase DEFAULT_SEARCH_LIST_SIZE to 500

**Pros:**

- One-line change
- Immediate effect

**Cons:**

- Wastes resources on small datasets
- Doesn't help existing indexes (metadata baked in)
- No per-query control

**Status:** REJECTED - insufficient, though will increase pruning_alpha as part of fix

## Tasks

### Phase: Test-First Development

- [x] Create `tests/ts/recall-scaling.test.ts` (COMPLETE)
  - [x] Test 1: 5k vectors, verify recall should be >80% with default beam
  - [x] Test 2: 5k vectors, verify recall improves to >90% with searchListSize=200
  - [x] Test 3: 2k vectors, demonstrate recall improvement with wider beam
  - [x] Tests fail with TypeScript errors (lines 183, 228): "Expected 3-4 arguments, but got 5"
  - [x] Expected failures confirm searchNearest() doesn't accept options parameter

### Phase: Implementation

- [ ] **Fix 1:** TypeScript API (1 hour)
  - [ ] Add `SearchOptions` interface with `searchListSize?`
  - [ ] Update `searchNearest()` signature (line 262, `src/index.ts`)
  - [ ] Add `search_list_size` constraint to SQL if provided
  - [ ] Update benchmark runner to use options object

- [ ] **Fix 2:** C vtab support (2 hours)
  - [ ] Add `DISKANN_IDX_SEARCH_LIST_SIZE` constraint (line 70, `diskann_vtab.c`)
  - [ ] Detect constraint in `diskannBestIndex()`
  - [ ] Read value in `diskannFilter()` and override `idx->search_list_size`
  - [ ] Alternative: Add `diskann_search_ex()` with options struct

- [ ] **Fix 3:** Increase pruning_alpha (5 minutes)
  - [ ] Change `DEFAULT_PRUNING_ALPHA` from 1.2 to 1.4 (line 27, `diskann_api.c`)
  - [ ] Less aggressive pruning → better connectivity

- [ ] **Fix 4:** Minimum degree enforcement (1 hour)
  - [ ] Add `const int MIN_DEGREE = 8;` in `prune_edges()` (line 86, `diskann_insert.c`)
  - [ ] Check `if (n_edges <= MIN_DEGREE) break;` before pruning
  - [ ] Prevents graph fragmentation

### Phase: Integration

- [ ] Update benchmark runner to use new API properly
- [ ] Verify existing 175 tests still pass
- [ ] Run `npm run bench:quick` - expect 95-99% recall

### Phase: Cleanup & Documentation

- [ ] Remove debug logging if any
- [ ] Update MEMORY.md with findings
- [ ] Move benchmark TPP to `_done/`

### Phase: Final Review

- [ ] All C tests pass: `make test && make asan && make valgrind`
- [ ] TypeScript recall test passes
- [ ] Benchmark quick profile shows 95-99% recall
- [ ] Optional: Run standard profile to verify 100k recall >85%

**Verification:**

```bash
# Run recall test (should pass after fixes)
cd tests/ts
npm test test_recall_scaling

# Run C tests
make clean && make test

# Run quick benchmark
cd benchmarks
npm run bench:quick

# Expected: 95-99% recall on small-64d-10k
```

## Notes

### Session 2026-02-10 (Research & Planning)

**Explored codebase with 3 parallel agents:**

1. Benchmark framework - found TypeScript parameter mismatch
2. Search implementation - found hardcoded beam width and random entry point
3. Graph construction - found aggressive pruning and no min degree

**Key insight:** This is NOT a single bug, but a combination of:

- API design gap (missing parameter)
- Default tuning for small datasets (beam=100, alpha=1.2)
- Missing safety checks (min degree)

At 10k scale, defaults are fine. At 100k scale, all three issues compound.

**Implementation priority:**

1. TypeScript API (unblocks benchmark parameter)
2. C constraint support (enables per-query tuning)
3. Pruning alpha increase (improves graph quality)
4. Min degree enforcement (prevents catastrophic fragmentation)

**Test strategy validated:**

- 5000 vectors sufficient to show issue
- 64 dimensions for speed
- Ground truth via brute force (reuse benchmark pattern)
- Run time <30s enables TDD cycle

### Planning Agents Used

- **Agent af35a6a** - Benchmark framework investigation
  - Found TypeScript API missing parameter
  - Traced benchmark runner attempting to pass ignored parameter
  - Identified hardcoded default fallback

- **Agent add855d** - Search implementation analysis
  - Confirmed beam width hardcoded to 100
  - Analyzed random entry point strategy
  - Explained why 0.1% exploration insufficient

- **Agent a5e5909** - Graph construction investigation
  - Found aggressive pruning with alpha=1.2
  - Identified no minimum degree enforcement
  - Explained cascading fragmentation at scale

- **Agent a43a146** - TDD approach design
  - Designed fast failing test strategy
  - Prioritized fixes by impact/risk
  - Defined success metrics

- **Agent ae2603d** - C-level fixes design
  - Detailed implementation for per-query beam width
  - Designed minimum degree enforcement
  - Provided adaptive scaling alternative

All agents completed successfully with comprehensive findings.

### Session 2026-02-10 (Test-First Development)

**Created failing test:** `tests/ts/recall-scaling.test.ts`

**Test structure:**

- 3 test cases covering recall at scale
- Uses 2k-5k vectors (fast enough for TDD cycle)
- 64 dimensions for speed
- Ground truth via brute force L2 distance
- Reproducible random vectors with LCG seeding

**Test failures confirmed:**

- TypeScript compilation errors at lines 183, 228
- Error: "Expected 3-4 arguments, but got 5"
- Confirms searchNearest() doesn't accept options parameter
- Tests 2 & 3 can't compile until TypeScript API fixed

**Test 1 expectation:**

- Will run when TypeScript errors fixed (doesn't use options)
- Expected to fail with ~50-70% recall (not >80%) due to:
  - Default beam width 100 too small for 5k vectors
  - Aggressive pruning (alpha=1.2) creating poor connectivity

**Next phase:** Implement TypeScript API fix (add SearchOptions parameter)

### Session 2026-02-10 (Implementation)

**Completed all 4 fixes:**

✅ **Fix 1: TypeScript API** (30 minutes)

- Added `SearchOptions` import from types.ts (already existed!)
- Updated `searchNearest()` to accept optional `options?: SearchOptions` parameter
- Add `search_list_size` SQL constraint when `options.searchListSize` provided
- Re-exported `SearchOptions` type from index.ts
- TypeScript compilation successful

✅ **Fix 2: C vtab support** (90 minutes)

- Added `DISKANN_IDX_SEARCH_LIST_SIZE 0x20` constraint flag
- Added `DISKANN_COL_SEARCH_LIST_SIZE 3` column definition
- Shifted `DISKANN_COL_META_START` from 3 to 4
- Updated xBestIndex to detect `search_list_size` constraint
- Updated xFilter to read value and temporarily override `idx->search_list_size`
- Properly restore original value after search completes
- Added `search_list_size HIDDEN` to schema declaration
- Added case in diskannColumn() for the new column
- Added to reserved column names

✅ **Fix 3: Increase pruning_alpha** (2 minutes)

- Changed `DEFAULT_PRUNING_ALPHA` from 1.2 to 1.4
- Less aggressive pruning for better connectivity at scale

✅ **Fix 4: Minimum degree enforcement** (10 minutes)

- Added `const int MIN_DEGREE = 8` in `prune_edges()`
- Check `if (n_edges <= MIN_DEGREE) break;` before pruning
- Prevents graph fragmentation at scale

**Current status:**

- C code compiles successfully
- 175 tests: 14 failures (all metadata-related + 1 alpha value check)
- Recall test compiles (was failing with TypeScript errors before)
- Need to investigate metadata column failures (likely related to META_START shift)

**Next:** Two parallel tracks (can be assigned to different engineers)

## Parallel Work Tracks

### Track A: Fix Metadata Test Failures (1-2 hours)

**Goal:** Resolve 13 metadata-related test failures caused by shifting DISKANN_COL_META_START from 3→4

**Problem:** Adding `search_list_size` as hidden column 3 shifted metadata columns from starting at index 3 to index 4. This broke existing metadata tests.

**Failing tests:**

```
test_open_index_computes_derived_fields - expects alpha=1.2, now 1.4 (EXPECTED)
test_vtab_meta_insert - metadata column returns NULL
test_vtab_meta_insert_partial - category='landscape' not found
test_vtab_meta_search_returns_cols - metadata not returned
test_vtab_meta_reopen - metadata lost after reopen
test_vtab_filter_eq - filter by category not working
test_vtab_filter_eq_other - filter returns 0 results
test_vtab_filter_gt - numeric filter not working
test_vtab_filter_lt - filter returns 0 results
test_vtab_filter_between - filter returns 0 results
test_vtab_filter_multi - combined filters return 0
test_vtab_filter_ne - NOT EQUAL filter returns 0
test_vtab_filter_recall - filtered search recall 0%
test_vtab_filter_graph_bridge - graph bridging not working
```

**Root cause investigation steps:**

1. **Check xUpdate (INSERT) path:**
   - File: `src/diskann_vtab.c`, function `diskannUpdate()`
   - Search for: `DISKANN_COL_META_START`
   - Verify metadata columns are written to correct indices in `_attrs` table
   - Look for off-by-one errors in column offset calculations

2. **Check prepare_meta_stmt (SELECT path):**
   - File: `src/diskann_vtab.c`, around line 1019 `prepare_meta_stmt:`
   - Verify the SELECT statement selects correct columns from `_attrs`
   - Check if column count matches expected metadata count

3. **Check diskannColumn (column retrieval):**
   - File: `src/diskann_vtab.c`, line ~1145
   - Already updated: `int meta_idx = i - DISKANN_COL_META_START;`
   - This should be correct now, but verify logic

4. **Check xBestIndex filter constraint handling:**
   - File: `src/diskann_vtab.c`, line ~741
   - Check: `c->iColumn >= DISKANN_COL_META_START`
   - Verify filter offset calculation: `c->iColumn - DISKANN_COL_META_START`

**Expected fixes:**

- Likely 1-2 places where column offset math is wrong
- May need to update `_attrs` table column order or indexing
- Update test expectations for alpha=1.4 (line 582 in test_runner.c)

**Verification:**

```bash
make clean && make && make test
# Expected: 175 Tests 0 Failures
```

### Track B: Validate Recall Improvements (1-2 hours)

**Goal:** Run the recall scaling test to verify our fixes improved recall from 1% to >85%

**Background:**

- Small dataset (10k): was 97% recall ✅
- Medium dataset (100k): was 1% recall ❌
- Target: 85-95% recall on all dataset sizes

**Test execution steps:**

1. **Run TypeScript recall test:**

   ```bash
   cd /home/mrm/src/sqlite-diskann
   npm test -- recall-scaling
   ```

   **Expected results:**
   - Test 1 (5k vectors, default beam): >80% recall
   - Test 2 (5k vectors, searchListSize=200): >90% recall
   - Test 3 (2k vectors, beam width comparison): >15% improvement

   **If tests fail:**
   - Document actual recall percentages
   - Check if TypeScript API properly passes searchListSize through SQL
   - Verify C vtab correctly reads and applies the constraint
   - Add debug logging in xFilter to confirm override happens

2. **Run benchmark quick profile:**

   ```bash
   cd benchmarks
   npm run bench:quick
   ```

   **Expected results:**
   - small-64d-10k: 95-99% recall (was already good)
   - Should remain 95-99% after fixes

3. **Optional: Run benchmark standard profile (if time permits):**

   ```bash
   cd benchmarks
   npm run bench:standard  # Takes ~20 minutes
   ```

   **Expected results:**
   - medium-256d-100k: 85-95% recall (was 1% before!)

**Debug procedures if recall doesn't improve:**

A. **Verify TypeScript → C parameter passing:**

```typescript
// Add debug logging in src/index.ts searchNearest()
console.log("searchListSize:", options?.searchListSize);
```

B. **Verify C constraint detection:**

```c
// Add debug in diskann_vtab.c xFilter() around line 891
fprintf(stderr, "search_list_size override: %u → %u\n",
        saved_search_list_size, pVtab->idx->search_list_size);
```

C. **Verify search actually uses wider beam:**

```c
// Add debug in diskann_search.c diskann_search_ctx_init()
fprintf(stderr, "Search beam width: %d\n", search_list_size);
```

**Success criteria:**

- ✅ Test 1: >80% avg recall on 5k vectors
- ✅ Test 2: >90% avg recall with searchListSize=200
- ✅ Test 3: >15% improvement with wider beam
- ✅ Quick benchmark: maintains 95-99% recall
- ✅ Standard benchmark (optional): achieves 85-95% recall on 100k

**Deliverable:**

- Document actual recall percentages achieved
- Update TPP with results
- If successful, update MEMORY.md with findings

## Critical Files

1. **`src/index.ts`** (lines 262-297) - Add searchListSize parameter
2. **`src/diskann_vtab.c`** (lines 560-850) - Support search_list_size constraint
3. **`src/diskann_api.c`** (line 27) - Increase DEFAULT_PRUNING_ALPHA to 1.4
4. **`src/diskann_insert.c`** (lines 86-123) - Add MIN_DEGREE enforcement
5. **`tests/ts/test_recall_scaling.test.ts`** (NEW) - Fast recall test
6. **`benchmarks/src/runners/diskann-runner.ts`** (line 94) - Use options object

## Expected Outcomes

**Before fixes:**

- Test: recall ~30-50% @ 5k vectors with beam=100
- Benchmark: 1% recall @ 100k vectors

**After TypeScript API + C constraint:**

- Test: recall ~70-80% @ 5k with searchListSize=200

**After pruning alpha + min degree:**

- Test: recall >90% @ 5k vectors
- Benchmark: 85-95% recall @ 100k vectors

**Time Estimate:** 5-6 hours total

## Session Summary

### ✅ Completed (2-3 hours)

- [x] Phase 1-3: Research, Test Design, Implementation Design (via Plan agents)
- [x] Phase 4: Test-First Development (recall-scaling.test.ts created, fails as expected)
- [x] Phase 5: Implementation (all 4 fixes implemented and compiling)

### 🔄 In Progress (2-3 hours remaining)

- [ ] **Track A:** Fix 14 metadata test failures (assign to Engineer 1)
- [ ] **Track B:** Validate recall improvements (assign to Engineer 2)

### ⏳ Remaining

- [ ] Phase 6: Integration (verify existing 175 tests pass, run benchmarks)
- [ ] Phase 7: Cleanup & Documentation (update MEMORY.md, remove debug logging)
- [ ] Phase 8: Final Review (ASan, Valgrind, full test suite)

### Key Achievements

1. ✅ TypeScript API now accepts `searchListSize` parameter
2. ✅ C vtab supports `search_list_size` SQL constraint
3. ✅ Pruning alpha increased to 1.4 (less aggressive)
4. ✅ Minimum degree enforcement prevents graph fragmentation
5. ✅ All code compiles successfully
6. ✅ Recall test ready to run (was failing TypeScript compilation before)

### Known Issues

- 14 test failures (13 metadata + 1 alpha value check)
- Metadata failures likely due to DISKANN_COL_META_START shift
- Does not block recall testing (Track B can proceed independently)
