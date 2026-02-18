# DiskANN Performance Experiments

This directory contains structured records of all performance experiments conducted on sqlite-diskann. Each experiment documents the hypothesis, methodology, results, and conclusions to build institutional knowledge.

## Experiment Format

Each experiment is documented in a separate markdown file with the following structure:

```markdown
# Experiment: [Short Title]

**Date:** YYYY-MM-DD
**Engineer:** [Name]
**Status:** [Planned | Running | Complete | Abandoned]

## Hypothesis

What we believe will happen and why.

## Motivation

Why we're running this experiment. What problem are we trying to solve?

## Test Setup

- Parameters tested
- Dataset size and characteristics
- Hardware/environment
- Comparison baseline

## Expected Results

Quantitative predictions with reasoning.

## Actual Results

Raw data, tables, graphs. Link to benchmark output files.

## Analysis

What the results mean. Surprises? Confirmations?

## Conclusions

- What we learned
- Impact on defaults/recommendations
- Follow-up experiments needed

## Artifacts

- Benchmark profiles: `benchmarks/profiles/experiment-001-*.json`
- Results: `results/experiment-001-*.json`
- Graphs: `experiments/graphs/experiment-001-*.png`
```

## Experiment Index

| ID  | Date       | Title                                | Status   | Key Finding                                |
| --- | ---------- | ------------------------------------ | -------- | ------------------------------------------ |
| 001 | 2026-02-11 | Cache + Hash Set Optimization        | Complete | 37% build speedup from BLOB caching        |
| 002 | 2026-02-11 | insert_list_size Reduction (200→100) | Complete | Only 2% improvement due to cache masking   |
| 003 | 2026-02-14 | max_neighbors Impact on Recall       | Complete | searchListSize bottleneck; keep default=32 |
| 004 | 2026-02-12 | Scaling Test (10k→200k)              | Planned  | Find crossover vs brute-force              |
| 005 | 2026-02-12 | Block Size Fix at 100k               | Complete | 98% recall (maxDeg=64), 64% (maxDeg=32)    |

## Quick Start

### Document a New Experiment

```bash
# Create from template
cp experiments/template.md experiments/experiment-XXX-short-name.md

# Edit with your hypothesis and setup
vim experiments/experiment-XXX-short-name.md

# Run benchmark
cd benchmarks
npm run bench -- --profile=profiles/experiment-XXX.json > ../experiments/experiment-XXX-output.txt

# Update experiment file with results
```

### Search Past Experiments

```bash
# Find experiments testing specific parameters
grep -r "max_neighbors" experiments/

# Find experiments with high recall
grep -r "Recall.*9[5-9]%" experiments/

# List all completed experiments
grep -l "Status: Complete" experiments/*.md
```

## Experiment Guidelines

### Before Running

1. **Check for similar past experiments** - Don't repeat work
2. **Document hypothesis clearly** - Make predictions falsifiable
3. **Plan for automation** - Use benchmark profiles, not manual tests
4. **Estimate time/cost** - Large benchmarks can take hours

### During Execution

1. **Capture raw output** - Redirect to `experiment-XXX-output.txt`
2. **Save result JSON** - Link to timestamped result files
3. **Note anomalies** - Document anything unexpected immediately
4. **Take screenshots** - For interactive visualizations

### After Completion

1. **Update experiment status** - Mark as Complete
2. **Add to index** - Update table above with key finding
3. **Update docs** - If defaults change, update PARAMETERS.md
4. **Link from issues/PRs** - Reference experiment IDs in commits

## Analysis Tools

### Compare Experiments

```bash
# Compare build times across experiments
jq '.[] | {experiment: .name, build_time: .build_time}' \
  results/experiment-*.json

# Plot recall vs build time
python3 experiments/scripts/plot-pareto.py results/experiment-*.json
```

### Statistical Significance

```bash
# Run t-test between two experiments
python3 experiments/scripts/ttest.py \
  results/experiment-001.json \
  results/experiment-002.json
```

## Best Practices

### Hypothesis-Driven

❌ **Bad:** "Let's try max_neighbors=48 and see what happens"
✅ **Good:** "Hypothesis: Increasing max_neighbors from 32→48 will improve recall@10 from 95%→97% but increase index size by 50% and build time by 10%"

### Reproducible

- Use benchmark profiles (JSON configs)
- Document exact versions (git commit hash)
- Note hardware specs
- Seed random number generators

### Incremental

- Change one variable at a time (when possible)
- Build on previous experiments
- Reference prior work

### Shareable

- Write for future you (6 months from now)
- Assume reader doesn't have context
- Include enough detail to reproduce exactly

## Common Pitfalls

1. **Not documenting baseline** - Always measure before/after
2. **Cherry-picking results** - Document failures too
3. **Ignoring variance** - Run multiple trials, report stddev
4. **Confounding variables** - Did something else change? (OS update, etc.)
5. **Premature conclusions** - Correlation ≠ causation

## Templates

- `template.md` - Blank experiment template
- `template-param-sweep.md` - For parameter sweeps
- `template-scaling.md` - For dataset size scaling tests
- `template-regression.md` - For performance regression investigations
