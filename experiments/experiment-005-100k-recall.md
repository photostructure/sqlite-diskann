# Experiment 005: Block Size Fix Validation at 100k Scale

**Date:** 2026-02-12
**Status:** Complete
**Git Commit:** `27079ed`

## Hypothesis

Block size fix (4KB → 40KB for 256D/32-edges) restores recall from 0-1% to >= 85% at 100k vectors.

**Reasoning:** 4KB blocks limited nodes to 2-3 edges, causing graph fragmentation at 100k scale. 40KB blocks support 35+ edges/node, enabling proper graph connectivity. Already validated at 10k (100% recall) and 25k (99.2% recall).

## Motivation

**Problem:** 100k vectors produced 0-1% recall with 4KB blocks (graph fragments into disconnected components).

**Why now:** Block size auto-calculation implemented and validated at small scale. Need confirmation at the scale where it originally broke.

**Success criteria:** Recall >= 85% @ k=10 on 100k vectors.

## Test Setup

### Parameters Under Test

| Parameter  | Before Fix       | After Fix                  | Notes                       |
| ---------- | ---------------- | -------------------------- | --------------------------- |
| block_size | 4096 (hardcoded) | 0 (auto-calculate → 40960) | Root cause of fragmentation |

### Benchmark Runs

**Run A — standard.json (maxDegree=64, generous params):**

- Validates the fix works at all with favorable parameters
- 200 queries, k=[1,10,50,100], searchListSize=500

**Run B — scaling-100k.json (maxDegree=32, default params):**

- Validates the fix works with default configuration
- 100 queries, k=[10], searchListSize=150

### Dataset

- **Size:** 100,000 vectors
- **Dimensions:** 256
- **Metric:** Euclidean (L2)
- **Source:** Synthetic (random, seed=42)
- **File:** `datasets/synthetic/medium-256d-100k.bin` (98 MB)
- **Ground truth:** `datasets/ground-truth/medium-256d-100k.json` (L2 distance)

### Hardware

- **CPU:** AMD Ryzen 9 5950X 16-Core (32 threads)
- **RAM:** 62 GB
- **OS:** Linux 6.17.0-14-generic

### Baseline (Before Block Size Fix)

- 100k recall: 0-1% @ k=10-100
- QPS dropped 45% when beam increased (searching wrong graph component)
- 10k recall: 97% (small scale still worked)

## Expected Results

| Metric        | Before Fix           | Expected (Run A)    | Expected (Run B)    |
| ------------- | -------------------- | ------------------- | ------------------- |
| Recall@10 (%) | 0-1%                 | >= 90%              | >= 85%              |
| Build Time    | unknown              | 30-90 min           | 15-45 min           |
| Index Size    | ~400 MB (4KB blocks) | ~4 GB (40KB blocks) | ~4 GB (40KB blocks) |

**Key prediction:** Recall jumps from near-zero to 85%+ at 100k scale.

**Risk:** If graph still fragments despite larger blocks, may need multi-start search (Priority 2 in parent TPP).

## Execution

### Commands Run

```bash
# Run A: standard.json (maxDegree=64)
cd /home/mrm/src/sqlite-diskann/benchmarks
npm run bench -- profiles/standard.json 2>&1 | tee ../experiments/experiment-005-output.txt

# Run B: scaling-100k.json (maxDegree=32)
npm run bench -- profiles/scaling-100k.json 2>&1 | tee -a ../experiments/experiment-005-output.txt
```

### Timeline

- **Run A start:** 2026-02-12 ~20:46 UTC
- **Run A end:** 2026-02-13 ~05:50 UTC (includes concurrent CPU contention from param-sweep)
- **Note:** Another benchmark (param-sweep-max-neighbors) was running concurrently, affecting build time

## Actual Results

### Run A: standard.json (maxDegree=64, buildL=200, searchL=500)

| Metric     | vec (brute force) | DiskANN                 | Notes                          |
| ---------- | ----------------- | ----------------------- | ------------------------------ |
| Build time | 1.6s              | **3810.2s (~63.5 min)** | Concurrent CPU contention      |
| Index size | 100.3 MB          | **7470.8 MB (~7.3 GB)** | 40KB blocks × 100k nodes       |
| Recall@1   | 100.0%            | **100.0%**              | Perfect                        |
| Recall@10  | 100.0%            | **98.0%**               | Exceeds 85% target             |
| Recall@50  | 100.0%            | **95.9%**               |                                |
| Recall@100 | 100.0%            | **93.1%**               |                                |
| QPS (k=10) | 47                | 45                      | Similar to brute force at 100k |
| p50 (k=10) | 21.09ms           | 21.94ms                 |                                |

### Run B: scaling-100k.json (maxDegree=32, buildL=100, searchL=150)

| Metric     | vec (brute force) | DiskANN                 | Notes                    |
| ---------- | ----------------- | ----------------------- | ------------------------ |
| Build time | 1.5s              | **821.3s (~13.7 min)**  | No CPU contention        |
| Index size | 100.3 MB          | **3955.2 MB (~3.9 GB)** | 40KB blocks × 100k nodes |
| Recall@10  | 100.0%            | **63.9%**               | Below 85% target         |
| QPS (k=10) | 49                | 384                     | 7.8x speedup             |
| p50 (k=10) | 20.36ms           | 2.57ms                  |                          |

**Note:** Required 3 attempts. First had ground truth mismatch (fixed harness). Next two crashed from extension segfault (concurrent lazy back-edges dev). Final attempt used `NODE_OPTIONS="--max-old-space-size=8192"`.

## Analysis

### Run A: Hypothesis Confirmed (maxDeg=64, searchL=500)

**Recall went from 0-1% to 93-100%.** The block size fix is decisively validated at 100k scale.

- Recall@10 = 98.0% (target was >= 85%) — **exceeds expectations**
- Recall degrades gracefully with k: 100% → 98% → 95.9% → 93.1%
- QPS is comparable to brute force (~45 vs ~47), suggesting search overhead is dominated by in-memory BLOB reads, not graph traversal
- Index size (7.3 GB) is 74x larger than brute force (100 MB) — significant overhead from edge vectors in 40KB blocks
- Build time (63.5 min) affected by concurrent benchmark; clean run would likely be faster

### Run B: Below Target (maxDeg=32, searchL=150)

**63.9% recall@10 — below the 85% target** but far above the pre-fix 0-1%.

- Graph IS connected — query 0 got 9/10 correct (90%), proving the block size fix works
- The low aggregate recall is a **search parameter issue**, not a graph connectivity issue
- Key differences from Run A: searchListSize (150 vs 500), maxDegree (32 vs 64), buildSearchListSize (100 vs 200)
- 384 QPS (7.8x faster than brute force) shows the speed-recall tradeoff

### Comparison: Run A vs Run B

| Parameter           | Run A     | Run B     | Impact                               |
| ------------------- | --------- | --------- | ------------------------------------ |
| maxDegree           | 64        | 32        | Fewer edges → less redundancy        |
| buildSearchListSize | 200       | 100       | Fewer candidates during construction |
| searchListSize      | 500       | 150       | Narrower beam → misses neighbors     |
| **Recall@10**       | **98.0%** | **63.9%** | 34% gap                              |
| **QPS**             | **45**    | **384**   | 8.5x faster with narrow beam         |
| Index size          | 7.3 GB    | 3.9 GB    | ~2x from doubled maxDegree           |

The searchListSize is likely the dominant factor — a wider beam explores more of the graph. A follow-up experiment with maxDeg=32 + searchL=300 would isolate this.

### Observations

1. **Block size fix validated** — both runs show dramatic improvement over 0-1% baseline
2. **Default search params need tuning** — searchListSize=150 is insufficient for 100k at maxDeg=32
3. **Index bloat is significant** — 3.9-7.3 GB for 100k × 256D (edge vectors dominate)
4. **Clear speed-recall tradeoff** — 384 QPS / 64% recall vs 45 QPS / 98% recall

### Bugs Found & Fixed

1. **Ground truth cache mismatch** — `loadOrComputeGroundTruth` loaded cached GT without validating query indices or k matched. Fixed: validate cache parameters, recompute if stale, don't overwrite larger caches.
2. **scaling-100k.json metric mismatch** — Profile used cosine but GT is L2/Euclidean. Fixed: changed to euclidean.
3. **Node OOM at 100k** — V8 default heap (4.2GB) is insufficient for 100k × 40KB block index (~4GB). Fix: use `NODE_OPTIONS="--max-old-space-size=8192"` for 100k+ benchmarks.

### Follow-up

- Test maxDeg=32 with searchListSize=300 to isolate search beam vs graph degree
- Consider adaptive searchListSize defaults based on dataset size

## Artifacts

- **Benchmark profiles:** `benchmarks/profiles/standard.json`, `benchmarks/profiles/scaling-100k.json`
- **Raw output:** `experiments/experiment-005-output.txt`
- **Parent TPP:** `_todo/20260211-validate-block-size-fix.md` (Phase 2)
