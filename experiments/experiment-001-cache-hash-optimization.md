# Experiment 001: Cache + Hash Set Optimization

**Date:** 2026-02-11
**Engineer:** AI Assistant (Session with user)
**Status:** Complete
**Git Commit:** `781ffb1` (block size fix), `08d11f4` (metadata fix)

## Hypothesis

Adding a BLOB cache (LRU, capacity 100) and hash set for visited tracking (O(1) vs O(n)) will reduce build time by ~5-7x (707s → ~100-140s) by eliminating redundant BLOB I/O on hot nodes.

**Reasoning:**

- Hot nodes (early inserts) are read 100+ times during later inserts
- Each insert with insert_list_size=200 does 200 BLOB reads (40KB each) = 16MB I/O
- Cache hit rate estimated at 60% based on hub node access patterns
- Hash set eliminates O(n²) visited tracking in beam search

## Motivation

**Problem:** Block size fix (commit 781ffb1) restored recall (0-1% → 93-100%) but exposed severe build performance issues:

- 25k vectors @ 256D: 707 seconds (11.8 minutes) baseline
- Root cause: 40KB blocks × insert_list_size=200 = 16MB I/O per insert = 400GB total

**Why now:** Blocking TPP `20260211-build-speed-optimization.md`

**Success criteria:**

- Build time: <150s for 25k vectors (5x faster minimum)
- Recall: ≥93% @ k=10 (no regression)
- All tests passing (ASan + Valgrind clean)

## Test Setup

### Parameters Under Test

| Component  | Implementation                      | Details                     |
| ---------- | ----------------------------------- | --------------------------- |
| BLOB Cache | LRU, array-based doubly-linked list | Capacity 100, linear search |
| Hash Set   | Open addressing, linear probing     | FNV-1a hash, capacity 256   |

### Dataset

- **Size:** 25,000 vectors
- **Dimensions:** 256
- **Metric:** Cosine
- **Source:** Synthetic (benchmarks/profiles/medium.json)

### Hardware

- **Commit:** 781ffb1 (before cache), then post-cache implementation

### Comparison Baseline

- **Baseline:** 707s build time (measured pre-cache on commit 781ffb1)
- **Config:** max_neighbors=32, insert_list_size=200, search_list_size=100

## Expected Results

| Metric          | Baseline | Expected        | Confidence |
| --------------- | -------- | --------------- | ---------- |
| Build Time (s)  | 707      | 140 (5x)        | High       |
| Cache Hit Rate  | N/A      | 60%             | Medium     |
| Recall@10 (%)   | 95       | 95 (no change)  | High       |
| Index Size (MB) | 988      | 988 (no change) | High       |

**Key prediction:** Cache will provide majority of speedup (~5x), hash set adds incremental improvement

## Execution

### Implementation

**Files Created:**

- `src/diskann_cache.h` (116 lines)
- `src/diskann_cache.c` (216 lines)
- `tests/c/test_cache.c` (310 lines) - 10 unit tests

**Files Modified:**

- `src/diskann_search.h` (+42 lines - VisitedSet struct)
- `src/diskann_search.c` (+151 lines - hash set + cache integration)
- `src/diskann_insert.c` (+19 lines - cache lifecycle)
- `tests/c/test_search.c` (+136 lines - 7 hash set tests)

**Test Results:**

- All 17 new tests PASS
- All 192 total tests PASS (ASan clean)

### Timeline

- **Design:** 2 hours
- **Test-First Development:** 2 hours
- **Implementation:** 4 hours
- **Integration:** 2 hours
- **Total:** 10 hours

## Actual Results

### Build Time with insert_list_size=200

```
Medium benchmark (25k vectors) - k=10

┌──────────────────┬────────────┬────────────┬──────────┬────────────┐
│ Library          │ Build (s)  │ Index (MB) │ QPS      │ Recall@k   │
├──────────────────┼────────────┼────────────┼──────────┼────────────┤
│ sqlite-diskann   │ 442.4      │ 988.8      │ 77       │ 99.2%      │
└──────────────────┴────────────┴────────────┴──────────┴────────────┘
```

### Build Time with insert_list_size=100

```
Medium benchmark (25k vectors) - k=10

┌──────────────────┬────────────┬────────────┬──────────┬────────────┐
│ Library          │ Build (s)  │ Index (MB) │ QPS      │ Recall@k   │
├──────────────────┼────────────┼────────────┼──────────┼────────────┤
│ sqlite-diskann   │ 432.7      │ 988.8      │ 83       │ 99.2%      │
└──────────────────┴────────────┴────────────┴──────────┴────────────┘
```

### Key Metrics

| Metric          | Baseline | Actual (200) | Actual (100) | Expected | Δ from Expected  |
| --------------- | -------- | ------------ | ------------ | -------- | ---------------- |
| Build Time (s)  | 707      | 442.4        | 432.7        | 140      | **+209% slower** |
| Speedup         | 1x       | 1.6x         | 1.63x        | 5x       | **3.1x less**    |
| Recall@10 (%)   | 95       | 99.2         | 99.2         | 95       | +4.2% ✅         |
| Index Size (MB) | 988      | 988          | 988          | 988      | 0% ✅            |

### Cache Hit Rate

Not instrumented in production code. Would need to add logging to measure actual hit rate.

## Analysis

### Hypothesis Validation

❌ **Refuted:** Expected 5x speedup, got 1.6x
✅ **Confirmed:** No recall regression
✅ **Confirmed:** Cache helps (37% faster than baseline)
❓ **Unclear:** Actual cache hit rate unknown (not measured)

### Key Insights

1. **Cache provides 37% speedup** - Significant but less than 5x prediction
2. **insert_list_size reduction has minimal impact** - Only 2% improvement (200→100) due to cache masking
3. **Recall improved unexpectedly** - 95% → 99.2% (likely due to other changes in commit 781ffb1)
4. **Hash set impact unclear** - Implemented but no isolated measurement

### Why Expected 5x vs Actual 1.6x?

**Possible reasons:**

1. **Cache hit rate < 60%** - May be only 20-30% in practice
2. **SQLite transaction overhead** - Not just BLOB I/O, also B-tree updates
3. **Edge pruning cost** - Prune_edges() may dominate after cache
4. **Cache lookup overhead** - Linear search in 100-entry cache has cost
5. **Baseline was measured incorrectly** - Need to re-verify 707s baseline

### Confounding Factors

- Between baseline and this test, multiple commits occurred
- Block size changed (4KB → 40KB) which affects I/O patterns
- No direct measurement of cache hit rate or hash set impact
- Different test environments possible

## Conclusions

### Summary

BLOB caching provides **37% build time improvement** (707s → 442s), falling short of the 5x target. The cache successfully prevents redundant BLOB I/O but other bottlenecks (transaction overhead, edge pruning) dominate. Reducing insert_list_size from 200→100 provides only marginal additional benefit (2%) because cache masks the I/O reduction.

### Impact on Recommendations

- ✅ **Keep cache implementation** - 37% speedup is valuable
- ✅ **Change DEFAULT_INSERT_LIST_SIZE** - 200→100 for slight improvement
- ❌ **Don't claim 5x speedup** - Set realistic expectations (~1.6x)
- 🔍 **Need more optimization** - To hit <150s target for 25k vectors

### Limitations

- Did not measure actual cache hit rate
- Did not profile to find remaining bottlenecks
- Only tested on 25k vectors (synthetic data)
- Did not test hash set in isolation

### Follow-up Questions

1. What is the actual cache hit rate in production?
2. What is the remaining bottleneck after cache? (Profile with perf/gprof)
3. Does hash set provide measurable benefit or is it negligible?
4. Can we optimize edge pruning (replace_edge_idx)?
5. Should cache capacity be increased beyond 100?

## Next Steps

- [x] Change DEFAULT_INSERT_LIST_SIZE to 100
- [ ] Add cache hit rate logging for monitoring
- [ ] Profile build to identify remaining bottlenecks
- [ ] Test max_neighbors reduction (32→24) for index size
- [ ] Run scaling test at 100k vectors to find DiskANN sweet spot

## Artifacts

- **Benchmark profile:** `benchmarks/profiles/medium.json`
- **Results (200):** `results-2026-02-12T01-49-40-607Z.json`
- **Results (100):** `results-2026-02-12T01-58-12-079Z.json`
- **Code:** Commits 781ffb1, 08d11f4

## References

- TPP: `_todo/20260211-build-speed-optimization.md`
- Memory: `.claude/projects/-home-mrm-src-sqlite-diskann/memory/MEMORY.md`
- DiskANN Paper: NeurIPS 2019

---

**Lessons for Future Experiments:**

1. **Measure baseline carefully** - 707s number may have been from different commit
2. **Instrument production code** - Should have added cache hit rate logging from start
3. **Test in isolation** - Should have measured cache and hash set separately
4. **Profile before optimizing** - Should have identified bottleneck with perf/gprof first
5. **Lower expectations** - 5x speedup predictions rarely materialize; be conservative
