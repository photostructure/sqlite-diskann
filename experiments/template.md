# Experiment XXX: [Short Descriptive Title]

**Date:** YYYY-MM-DD
**Engineer:** [Your Name]
**Status:** Planned
**Related Issues:** #NNN
**Git Commit:** `git rev-parse HEAD`

## Hypothesis

[Clear, falsifiable prediction. Example: "Reducing max_neighbors from 32 to 24 will decrease index size by ~30% with <2% recall loss"]

**Reasoning:** [Why we believe this will happen based on theory/prior work]

## Motivation

**Problem:** [What issue are we trying to solve?]

**Why now:** [What triggered this experiment?]

**Success criteria:** [How will we know if this worked?]

## Test Setup

### Parameters Under Test

| Parameter     | Baseline | Test Value(s) | Range Rationale         |
| ------------- | -------- | ------------- | ----------------------- |
| max_neighbors | 32       | [24, 48, 64]  | Industry standard range |

### Dataset

- **Size:** 50,000 vectors
- **Dimensions:** 256
- **Metric:** Cosine
- **Source:** Synthetic (random, seed=42)

### Hardware

- **CPU:** [model, cores]
- **RAM:** [amount]
- **Disk:** [SSD/HDD, model]
- **OS:** [version]

### Comparison Baseline

- **Control:** Current defaults (max_neighbors=32, insert_list=100)
- **Baseline metrics:** [From previous experiment or current state]

### Benchmark Profile

`benchmarks/profiles/experiment-XXX-title.json`

## Expected Results

| Metric          | Baseline | Expected Change | Confidence |
| --------------- | -------- | --------------- | ---------- |
| Build Time (s)  | 432      | -50s (-12%)     | Medium     |
| Index Size (MB) | 988      | -296 (-30%)     | High       |
| Recall@10 (%)   | 99.2     | -1.5%           | Medium     |
| QPS             | 82       | +5 (+6%)        | Low        |

**Key prediction:** [Most important expected outcome]

**Risk:** [What could go wrong?]

## Execution

### Commands Run

```bash
cd benchmarks
npm run bench -- --profile=profiles/experiment-XXX-title.json > \
  ../experiments/experiment-XXX-output.txt 2>&1
```

### Timeline

- **Start:** [timestamp]
- **End:** [timestamp]
- **Duration:** [minutes]

## Actual Results

### Raw Data

[Link to output file or paste table here]

```
[Benchmark output table]
```

### Key Metrics

| Metric          | Baseline | Actual | Expected | Δ from Expected |
| --------------- | -------- | ------ | -------- | --------------- |
| Build Time (s)  | 432      | [X]    | 382      | [±N%]           |
| Index Size (MB) | 988      | [X]    | 692      | [±N%]           |
| Recall@10 (%)   | 99.2     | [X]    | 97.7     | [±N%]           |
| QPS             | 82       | [X]    | 87       | [±N%]           |

### Visualizations

[Attach graphs, screenshots]

### Anomalies

[Anything unexpected during execution]

## Analysis

### Hypothesis Validation

✅ **Confirmed:** [What matched predictions]
❌ **Refuted:** [What didn't match]
❓ **Unclear:** [Ambiguous results]

### Key Insights

1. [Most important finding]
2. [Secondary finding]
3. [Surprising discovery]

### Statistical Significance

- **Sample size:** N trials
- **Standard deviation:** [if multiple runs]
- **p-value:** [if comparison test run]

### Confounding Factors

[What else might have influenced results?]

## Conclusions

### Summary

[2-3 sentence summary of what we learned]

### Impact on Recommendations

- **Update defaults?** [Yes/No, which parameters, to what values]
- **Update documentation?** [Which files need changes]
- **User guidance?** [New recommendations for when to use these settings]

### Limitations

[What this experiment didn't test or answer]

### Follow-up Questions

1. [Question raised by results]
2. [Next thing to investigate]

## Next Steps

- [ ] Update `DEFAULT_X` in `src/diskann_api.c` if defaults changed
- [ ] Update `PARAMETERS.md` with findings
- [ ] Document in README.md use case guidance
- [ ] Run follow-up experiment XXX if needed
- [ ] Close related issue #NNN

## Artifacts

- **Benchmark profile:** `benchmarks/profiles/experiment-XXX-title.json`
- **Raw output:** `experiments/experiment-XXX-output.txt`
- **Results JSON:** `results/experiment-XXX-[timestamp].json`
- **Graphs:** `experiments/graphs/experiment-XXX-*.png`

## References

- Prior experiment: [Link to related experiment]
- Paper: [Relevant research if applicable]
- Discussion: [GitHub issue, PR, or internal doc]

---

**Lessons for Future Experiments:**

[What would you do differently next time? What worked well?]
