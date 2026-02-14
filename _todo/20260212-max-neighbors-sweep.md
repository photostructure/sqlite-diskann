# max_neighbors Parameter Sweep

## Summary

Run experiment-003 to find optimal `max_neighbors` default. Current default (32) creates 40KB blocks for 256D vectors. Reducing to 24 would cut block size to ~28KB (30% smaller). Test at 100k scale to ensure recall stays >= 93% target while minimizing index size and build time.

## Current Phase

- [x] Research & Planning
- [ ] Test Design
- [ ] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` — Project conventions
- `PARAMETERS.md` — Parameter mutability and recommendations
- `experiments/README.md` — Experiment documentation guidelines
- `experiments/template.md` — Experiment format
- `benchmarks/TUNING-GUIDE.md` — Benchmark methodology
- `src/diskann_api.c` — `DEFAULT_MAX_NEIGHBORS` and `calculate_block_size()`

## Description

**Problem:** 40KB blocks (256D/32-edges) create large indices and slow builds. Need to find optimal max_neighbors that balances recall, index size, and build time at 100k+ scale.

**Constraints:**

- Recall must stay >= 93% @ k=10
- No code changes to sweep — just benchmark config
- If default changes, existing indices need rebuild (immutable param)

**Success Criteria:**

- Identify optimal max_neighbors that keeps recall >= 93% while minimizing index size
- Document results in `experiments/experiment-003-max-neighbors.md`
- If 24 works: update `DEFAULT_MAX_NEIGHBORS` in `src/diskann_api.c`

## Tribal Knowledge

- Block size formula: `node_overhead + (margin × edge_overhead)` where margin = `max_neighbors * 1.1`
- Block size auto-calculation implemented in `calculate_block_size()` - produces 40KB for 256D/32-edges
- 256D/32-edges = 40KB blocks, 256D/24-edges = ~28KB blocks (30% smaller)
- `max_neighbors` is IMMUTABLE after index creation — changing default only affects new indices
- Previous 100k experiments showed 0.3% recall with old 4KB blocks — block size fix resolves this
- The param-sweep-max-neighbors.json profile uses 100k dataset (medium-256d-100k.bin)
- Experiment-003 file exists but has stale output from before block size fix — delete and re-run
- `insert_list_size` reduction gave only 2% improvement because cache masked I/O savings — max_neighbors reduction may have bigger impact since it changes block SIZE not just count

## Solutions

### Option 1: Reduce to 24 (Expected Best)

**Pros:** 30% smaller blocks, proportionally faster builds, still well above typical DiskANN recommendations (16-32)
**Cons:** Fewer edges = potentially lower recall at very large scale
**Status:** Test first

### Option 2: Keep at 32

**Pros:** Maximum connectivity, highest recall
**Cons:** Index size problem persists
**Status:** Baseline comparison

### Option 3: Reduce to 16

**Pros:** Smallest possible index
**Cons:** May drop recall significantly, DiskANN papers recommend >= 24 for good recall
**Status:** Test as lower bound

## Tasks

- [ ] Fix PARAMETERS.md default (change 64 → 32 for max_neighbors)
- [ ] Delete stale `experiments/experiment-003-output.txt` (from before block size fix)
- [ ] Verify benchmark dataset exists: `ls benchmarks/datasets/synthetic/medium*`
- [ ] If missing, generate: `cd benchmarks && npm run prepare`
- [ ] Investigate benchmark harness: why did previous run only execute 1/4 configs?
- [ ] Run sweep: `cd benchmarks && npm run bench -- --profile=profiles/param-sweep-max-neighbors.json 2>&1 | tee ../experiments/experiment-003-output.txt`
- [ ] Record results in experiment-003-max-neighbors.md: build time, recall@k (k=1,10,50,100), index size, QPS for each max_neighbors value
- [ ] Analyze: plot recall vs index size tradeoff
- [ ] If max_neighbors=24 maintains recall >= 93%: update `DEFAULT_MAX_NEIGHBORS` in `src/diskann_api.c`
- [ ] Run `make clean && make test` after any code change (must be 204/204)
- [ ] Update `experiments/README.md` index with final results
- [ ] Update `PARAMETERS.md` with measured results and new recommendations

**Verification:**

```bash
cd benchmarks
npm run bench -- --profile=profiles/param-sweep-max-neighbors.json
# Check output for recall >= 93% at preferred max_neighbors
make clean && make test  # If code changed
```

## Notes

(To be filled during execution)
