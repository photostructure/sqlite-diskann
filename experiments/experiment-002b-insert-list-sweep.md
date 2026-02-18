# Experiment 002b: insert_list_size Parameter Sweep

**Date:** 2026-02-11
**Engineer:** [Fill in your name]
**Status:** In Progress
**Git Commit:** `1398e9a`

## Hypothesis

There exists an optimal insert_list_size value where recall plateaus - increasing beyond this point wastes build time without improving recall.

**Reasoning:**

- insert_list_size controls candidate pool during graph construction
- Higher values = more exploration = better graph connectivity = higher recall
- But diminishing returns: recall plateaus when graph is "well-connected enough"
- libSQL uses 75, we currently use 100 (from Exp 001)
- Hypothesis: Recall plateaus around 100-150 for 50k vectors @ 256D

## Motivation

**Problem:** Build time directly proportional to insert_list_size. Need to find minimum value that achieves target recall (≥95%).

**Why now:** Exp 001 showed only 2% build time improvement from 200→100, suggesting we may already be near optimal. Need to validate across full parameter range.

**Success criteria:**

- Identify plateau point where recall stops improving
- Validate insert_list_size=100 is optimal (or find better default)
- Document recall vs build time tradeoff curve

## Test Setup

### Parameters Under Test

| Parameter        | Baseline | Test Values                       | Range Rationale            |
| ---------------- | -------- | --------------------------------- | -------------------------- |
| insert_list_size | 100      | [50, 75, 100, 150, 200]           | libSQL=75, old default=200 |
| dimensions       | 256      | (fixed)                           | Representative             |
| max_neighbors    | 32       | (fixed, may change after Exp 003) | Current default            |
| search_list      | 100      | (fixed)                           | Consistent with insert     |

### Dataset

- **Size:** 50,000 vectors
- **Dimensions:** 256
- **Metric:** Cosine
- **Source:** Synthetic (random, seed=42)

### Hardware

- **CPU:** AMD Ryzen 9 5950X (16 cores, 32 threads)
- **RAM:** 62 GB
- **Disk:** NVMe SSD (912GB capacity, 38% used)
- **OS:** Ubuntu 24.04, Linux 6.17.0-14-generic

### Comparison Baseline

- **Control:** insert_list_size=100 (current default)
- **Baseline:** From 25k @ insert_list=100 (Exp 001):
  - Build time: 432s
  - Recall@10: 99.2%
  - QPS: 82

### Benchmark Profile

`benchmarks/profiles/param-sweep-insert-list.json`

## Expected Results

| insert_list | Build Time (s) | Recall@10 (%) | QPS | Notes                          |
| ----------- | -------------- | ------------- | --- | ------------------------------ |
| 50          | 440 (−50%)     | 95-96%        | 90  | Too low? Graph may fragment    |
| 75          | 660 (−25%)     | 98%           | 87  | libSQL default, should be good |
| 100 (base)  | 880            | 99%           | 85  | Current default                |
| 150         | 1320 (+50%)    | 99.2%         | 82  | Diminishing returns start      |
| 200         | 1760 (+100%)   | 99.5%         | 80  | Plateau - marginal improvement |

**Key prediction:** Recall plateaus between 100-150. Optimal is likely 75-100 range.

**Risk:**

- insert_list=50 may be too aggressive, causing recall <95%
- I/O contention from parallel experiments may affect build times
- Cache (from Exp 001) may mask build time differences

## Execution

### Commands Run

```bash
cd /home/mrm/src/sqlite-diskann-experiments/exp002b-insert-list
cd benchmarks
rm -rf datasets/synthetic/*.db
npm install --ignore-scripts  # Done already
# Fix symlink: Already done
npm run prepare
date && npm run bench -- --profile=profiles/param-sweep-insert-list.json 2>&1 | \
  tee ../experiments/experiment-002b-output.txt && date
```

### Timeline

- **Start:** [Fill in timestamp]
- **End:** [Fill in timestamp]
- **Duration:** [Expected: 25-35 minutes]

## Actual Results

### Raw Data

See `experiments/experiment-002b-output.txt` for full benchmark output.

```
[Paste results table from benchmark]
```

### Key Metrics

| insert_list | Build Time (s) | Recall@10 (%) | QPS | Δ from Expected |
| ----------- | -------------- | ------------- | --- | --------------- |
| 50          | [X]            | [X]%          | [X] | [±N%]           |
| 75          | [X]            | [X]%          | [X] | [±N%]           |
| 100 (base)  | [X]            | [X]%          | [X] | [±N%]           |
| 150         | [X]            | [X]%          | [X] | [±N%]           |
| 200         | [X]            | [X]%          | [X] | [±N%]           |

### Recall Plateau Analysis

[Plot or describe where recall stops improving significantly]

**Plateau point:** insert_list=[X] (recall stops improving beyond this)

### Build Time Efficiency

[Calculate recall improvement per second of build time]

| insert_list | Recall/BuildTime Ratio | Efficiency vs 100 |
| ----------- | ---------------------- | ----------------- |
| 50          | [X]                    | [±N%]             |
| 75          | [X]                    | [±N%]             |
| 100         | [X]                    | baseline          |
| 150         | [X]                    | [±N%]             |
| 200         | [X]                    | [±N%]             |

### Anomalies

[Note anything unexpected]

## Analysis

### Hypothesis Validation

✅ **Confirmed:** [What matched predictions about plateau point]
❌ **Refuted:** [What didn't match]
❓ **Unclear:** [Ambiguous results]

### Key Insights

1. **Optimal value:** [Best insert_list_size for recall vs build time tradeoff]
2. **Plateau behavior:** [At what point does recall stop improving?]
3. **libSQL comparison:** [Is their 75 value justified by our data?]

### Confounding Factors

- Parallel experiments (exp003, exp004) - I/O contention
- Cache enabled (from Exp 001) - may mask I/O-based build time differences
- Dataset size 50k vs baseline 25k - not direct comparison
- [Any other factors]

## Conclusions

### Summary

[2-3 sentences: What's the optimal insert_list_size? Should we change the default?]

### Impact on Recommendations

- **Update defaults?**
  - If optimal != 100: Change `DEFAULT_INSERT_LIST_SIZE` in `src/diskann_api.c:25`
  - Document reasoning in code comment

- **Update documentation:**
  - Update PARAMETERS.md with recall plateau data
  - Add guidance: "Use insert_list=X for datasets <Yk, Y for >Yk"

- **User guidance:**
  - Fast build (lower recall): insert_list=50-75
  - Balanced (recommended): insert_list=[optimal value]
  - Maximum recall (slow): insert_list=150-200

### Limitations

- Only tested synthetic data at one scale (50k)
- Cache may be masking true I/O cost differences
- Real embeddings may have different connectivity requirements

### Follow-up Questions

1. Does optimal insert_list_size vary with dataset size? (test at 100k, 200k)
2. Does max_neighbors (Exp 003 result) affect optimal insert_list?
3. Can we predict optimal value analytically from dimensions and dataset size?

## Next Steps

- [ ] If optimal != 100: Update `DEFAULT_INSERT_LIST_SIZE` in `src/diskann_api.c`
- [ ] Update `PARAMETERS.md` with plateau curve
- [ ] Document recall vs build time tradeoff in README.md
- [ ] Consider combined sweep: (max_neighbors, insert_list) grid search
- [ ] Update experiments/README.md index

## Artifacts

- **Benchmark profile:** `benchmarks/profiles/param-sweep-insert-list.json`
- **Raw output:** `experiments/experiment-002b-output.txt`
- **Results JSON:** `benchmarks/results/results-*.json` (5 files)

## References

- Prior experiment: experiment-001 (established insert_list=100 default)
- libSQL default: insert_list=75 (source: libSQL codebase)
- TPP: `_todo/20260211-build-speed-optimization.md`

---

**Lessons for Future Experiments:**

[After completion, note insights about parameter sweeps, identifying plateaus, etc.]
