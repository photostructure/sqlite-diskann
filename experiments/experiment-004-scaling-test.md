# Experiment 004: Scaling Test - DiskANN vs Brute Force Crossover

**Date:** 2026-02-11
**Engineer:** [Fill in your name]
**Status:** In Progress
**Git Commit:** `1398e9a`

## Hypothesis

DiskANN becomes competitive with brute-force (sqlite-vec) at ~75k-100k vectors due to logarithmic vs linear scaling.

**Reasoning:**

- DiskANN search: O(log n) graph traversal
- Brute-force: O(n) linear scan (SIMD-optimized)
- sqlite-vec QPS observed: 206 @ 25k vectors
- DiskANN QPS observed: 82 @ 25k vectors (2.5x slower)
- As n grows, O(log n) advantage should overcome constant-factor overhead
- Expected crossover: ~75k-100k vectors

## Motivation

**Problem:** DiskANN overhead only justified for large datasets. Need to know when to recommend it vs brute-force.

**Why now:** User adoption requires clear guidance: "Use DiskANN when you have >N vectors, otherwise use sqlite-vec."

**Success criteria:**

- Identify crossover point where DiskANN QPS ≥ sqlite-vec QPS
- Measure scaling curves for both approaches
- Extrapolate to 500k, 1M to prove DiskANN's value at scale

## Test Setup

### Parameters Under Test

| Parameter     | Value                       | Rationale                                  |
| ------------- | --------------------------- | ------------------------------------------ |
| dataset size  | [10k, 25k, 50k, 100k, 200k] | Logarithmic progression                    |
| dimensions    | 256                         | Representative text embedding              |
| k             | [10, 50, 100]               | Common query sizes                         |
| max_neighbors | 32                          | Current default (may change after Exp 003) |
| insert_list   | 100                         | From Exp 001 tuning                        |
| search_list   | 150                         | Wider beam for better recall               |

### Dataset

- **Sizes:** 10,000 / 25,000 / 50,000 / 100,000 / 200,000 vectors
- **Dimensions:** 256
- **Metric:** Cosine
- **Source:** Synthetic (random, seed=42)

### Hardware

- **CPU:** AMD Ryzen 9 5950X (16 cores, 32 threads)
- **RAM:** 62 GB
- **Disk:** NVMe SSD (912GB capacity, 38% used)
- **OS:** Ubuntu 24.04, Linux 6.17.0-14-generic

### Comparison Baseline

- **DiskANN:** Current optimized config (cache enabled, insert_list=100)
- **sqlite-vec:** Brute-force with SIMD optimizations

### Benchmark Profile

`benchmarks/profiles/scaling-test.json`

## Expected Results

### QPS Scaling

| Dataset Size | DiskANN QPS | sqlite-vec QPS | Winner      | Margin |
| ------------ | ----------- | -------------- | ----------- | ------ |
| 10k          | 60          | 220            | sqlite-vec  | 3.7x   |
| 25k          | 82          | 206            | sqlite-vec  | 2.5x   |
| 50k          | 95          | 180            | sqlite-vec  | 1.9x   |
| 100k         | 110         | 140            | sqlite-vec  | 1.3x   |
| 200k         | 125         | 90             | **DiskANN** | 1.4x   |

**Key prediction:** Crossover happens between 100k-200k vectors. At 200k, DiskANN should be 1.3-1.5x faster.

### Build Time Scaling

| Dataset Size | Build Time (s) | Index Size (MB) | MB/s Write |
| ------------ | -------------- | --------------- | ---------- |
| 10k          | 70             | 400             | 5.7        |
| 25k          | 432            | 1000            | 2.3        |
| 50k          | 1200           | 2000            | 1.7        |
| 100k         | 3200           | 4000            | 1.25       |
| 200k         | 8000           | 8000            | 1.0        |

**Risk:**

- Build time may be prohibitive at 200k (2+ hours)
- I/O contention from parallel experiments
- Memory pressure at 200k vectors (~8GB index + cache)

## Execution

### Commands Run

```bash
cd /home/mrm/src/sqlite-diskann-experiments/exp004-scaling
cd benchmarks
rm -rf datasets/synthetic-*.db
npm install
npm run prepare  # May take 10-15 min for 200k dataset
date && npm run bench -- --profile=profiles/scaling-test.json 2>&1 | \
  tee ../experiments/experiment-004-output.txt && date
```

### Timeline

- **Start:** [Fill in timestamp]
- **End:** [Fill in timestamp]
- **Duration:** [Expected: 60-90 minutes]

## Actual Results

### Raw Data

See `experiments/experiment-004-output.txt` for full benchmark output.

```
[Paste scaling results table]
```

### QPS Scaling (Actual)

| Dataset Size | DiskANN QPS | sqlite-vec QPS | Winner | Margin | Δ from Expected |
| ------------ | ----------- | -------------- | ------ | ------ | --------------- |
| 10k          | [X]         | [X]            | [X]    | [X]    | [±N%]           |
| 25k          | [X]         | [X]            | [X]    | [X]    | [±N%]           |
| 50k          | [X]         | [X]            | [X]    | [X]    | [±N%]           |
| 100k         | [X]         | [X]            | [X]    | [X]    | [±N%]           |
| 200k         | [X]         | [X]            | [X]    | [X]    | [±N%]           |

### Build Time Scaling (Actual)

| Dataset Size | Build Time (s) | Index Size (MB) | MB/s Write | Δ from Expected |
| ------------ | -------------- | --------------- | ---------- | --------------- |
| 10k          | [X]            | [X]             | [X]        | [±N%]           |
| 25k          | [X]            | [X]             | [X]        | [±N%]           |
| 50k          | [X]            | [X]             | [X]        | [±N%]           |
| 100k         | [X]            | [X]             | [X]        | [±N%]           |
| 200k         | [X]            | [X]             | [X]        | [±N%]           |

### Recall@k (Verify no degradation at scale)

| Dataset Size | Recall@10 | Recall@50 | Recall@100 |
| ------------ | --------- | --------- | ---------- |
| 10k          | [X]%      | [X]%      | [X]%       |
| 25k          | [X]%      | [X]%      | [X]%       |
| 50k          | [X]%      | [X]%      | [X]%       |
| 100k         | [X]%      | [X]%      | [X]%       |
| 200k         | [X]%      | [X]%      | [X]%       |

### Anomalies

[Note anything unexpected]

## Analysis

### Hypothesis Validation

✅ **Confirmed:** [What matched predictions about crossover point]
❌ **Refuted:** [What didn't match - e.g., if crossover earlier/later than expected]
❓ **Unclear:** [Ambiguous results]

### Key Insights

1. **Crossover point:** [Actual dataset size where DiskANN becomes faster]
2. **Scaling behavior:** [How closely does it match O(log n) vs O(n)?]
3. **Build time viability:** [Is 200k build time acceptable for production?]

### Extrapolation to Large Scale

Using measured scaling curves, predict:

| Dataset Size | DiskANN QPS (predicted) | sqlite-vec QPS (predicted) | Margin |
| ------------ | ----------------------- | -------------------------- | ------ |
| 500k         | [Calculate]             | [Calculate]                | [X]    |
| 1M           | [Calculate]             | [Calculate]                | [X]    |

**Method:** [Describe curve fitting - linear for sqlite-vec, log for DiskANN]

### Confounding Factors

- Parallel experiments (exp003, exp002b) running - I/O contention
- Cache size (100 entries) may become limiting at 200k
- SQLite page cache settings (not tuned for large datasets)
- [Any other factors]

## Conclusions

### Summary

[2-3 sentences: At what scale is DiskANN worth the complexity? What's the recommendation?]

### Impact on Recommendations

- **Update README.md:**
  - "Use sqlite-vec (brute-force) for <Xk vectors"
  - "Use DiskANN for >Xk vectors"
  - Document crossover point clearly

- **Update PARAMETERS.md:**
  - Scaling guidance by dataset size
  - Expected build times at various scales

- **Marketing/docs:**
  - "DiskANN handles 500k+ vectors efficiently"
  - Provide performance comparison chart

### Limitations

- Only tested synthetic data (real embeddings may have different connectivity)
- Single metric (cosine) - other metrics may behave differently
- Fixed dimensions (256) - higher/lower dims may shift crossover
- No testing beyond 200k (extrapolation not validated)

### Follow-up Questions

1. Can we optimize build time for 200k+ datasets? (parallel insertion?)
2. Does crossover point change with different max_neighbors? (after Exp 003)
3. What happens at 500k, 1M with real data?
4. Can we auto-select algorithm based on dataset size?

## Next Steps

- [ ] Update README.md with dataset size recommendations
- [ ] Update PARAMETERS.md with scaling guidance
- [ ] Document crossover point in user-facing docs
- [ ] Consider Exp 005: Validate extrapolation at 500k (if crossover confirmed)
- [ ] Update experiments/README.md index

## Artifacts

- **Benchmark profile:** `benchmarks/profiles/scaling-test.json`
- **Raw output:** `experiments/experiment-004-output.txt`
- **Results JSON:** `benchmarks/results/results-*.json` (5 files, one per dataset size)

## References

- Prior experiment: experiment-001 (established 25k baseline)
- O(log n) analysis: DiskANN paper (Microsoft Research)
- sqlite-vec performance: https://github.com/asg017/sqlite-vec

---

**Lessons for Future Experiments:**

[After completion, note insights about running long experiments, extrapolation methods, etc.]
