# Deprecate sqlite-diskann and Migrate Assets to sqlite-vec

## Summary

Benchmarks show sqlite-diskann has prohibitive performance issues (2,268x slower builds, 74x larger indexes than sqlite-vec, similar QPS). Gracefully deprecate the npm package, archive the repository, and migrate valuable benchmark infrastructure and research findings to sqlite-vec.

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

- `benchmarks/src/harness.ts` - Multi-library comparison framework
- `benchmarks/src/runners/usearch-runner.ts` - USearch integration
- `experiments/README.md` - Experiment methodology
- `experiments/experiment-005-100k-recall.md` - Post-mortem findings
- `package.json` - npm package configuration
- `../sqlite-vec/` - Target repository for migration

## Description

**Problem:** sqlite-diskann is not viable compared to alternatives:
- Build time: 3626s vs 1.6s (sqlite-vec) = 2,268x slower
- Index size: 7.5 GB vs 100 MB (sqlite-vec) = 74x larger
- QPS: ~46 (same as sqlite-vec's brute force)
- Only advantage: recall is good (93-100%), but not enough to offset costs

**Constraints:**
- Package already published to npm (v0.1.2, 2 versions total)
- Users may have installed it (minimize disruption)
- Valuable assets should be preserved (benchmark infrastructure, research)
- sqlite-vec has minimal benchmark infrastructure (just Rust micro-benchmarks)

**Success Criteria:**
- [ ] New repo `node-vector-bench` created with benchmark infrastructure
- [ ] npm package deprecated with clear explanation and guidance
- [ ] Post-mortem narrative explains what happened and why
- [ ] Experiment methodology preserved in new repo
- [ ] sqlite-diskann repo archived with deprecation notice
- [ ] All links work, no orphaned docs

## Tribal Knowledge

**npm Package Details:**
- Package: `@photostructure/sqlite-diskann`
- Current version: 0.1.2
- Published: 5 days ago by GitHub Actions
- Downloads: Unknown (likely minimal given recent publish)
- Includes: prebuilds/, dist/, TypeScript bindings

**Benchmark Infrastructure:**
- Self-contained in `benchmarks/` subdirectory with own package.json
- Multi-library runner supporting diskann, vec, usearch
- USearch integration provides HNSW comparison baseline
- Profile-based configuration (JSON)
- Ground truth computation and caching
- Statistical metrics (recall@k, percentile latencies, QPS)
- Multiple reporters (console table, JSON, markdown)
- Dataset generation utilities

**Experiment Methodology:**
- Template-driven documentation (`experiments/template.md`)
- Hypothesis-driven testing approach
- Structured experiment index
- Guidelines document (`experiments/README.md`)
- Valuable for any performance engineering work

**sqlite-vec Current State:**
- Has `benchmarks/` directory but only Rust micro-benchmarks
- Empty README.md in benchmarks/
- No multi-library comparison infrastructure
- Would benefit from this benchmark harness

**Key Findings to Preserve:**
- Block size limitations (4KB → 2-3 edges max for 256D)
- Graph fragmentation at scale
- BLOB I/O overhead dominates
- Write performance bottlenecks (profiling data)
- Why DiskANN's advantages don't materialize in SQLite

## Solutions

### Option 1: Standalone Benchmark Repo (Recommended)

**Create new repo: `node-vector-bench`**
- Implementation-agnostic vector search benchmarking framework
- Multi-library runner supporting any vector library (diskann, vec, usearch, hnswlib, etc.)
- Profile-based configuration, ground truth computation, statistical reporting
- Experiment methodology and templates
- Not tied to any specific implementation

**Migrate from sqlite-diskann:**
- Benchmark harness (`benchmarks/src/`, `benchmarks/package.json`)
- USearch runner (as reference competitor)
- Add sqlite-vec runner (migrate from existing)
- Experiment methodology (`experiments/template.md`, `experiments/README.md`)

**Deprecate sqlite-diskann:**
- Publish v0.2.0 with deprecation notice and clear narrative of why it failed
- Update package.json with `"deprecated"` field
- Add prominent deprecation banner to README with link to post-mortem
- Archive GitHub repo (read-only)
- Keep public as learning resource

**Pros:**
- Preserves valuable benchmark work in neutral location
- Benefits entire Node.js vector search ecosystem
- Not tied to any vendor or implementation
- sqlite-vec can use it without inheriting diskann baggage
- Clear migration path for users
- Post-mortem becomes valuable reference

**Cons:**
- Need to create new repo
- Some work to make runners pluggable
- Need to document runner interface

**Status:** Recommended approach

### Option 2: Migrate to sqlite-vec

**Pros:**
- Simpler - one destination
- sqlite-vec gets better benchmarks

**Cons:**
- sqlite-vec already has its own benchmark infrastructure
- Ties universal benchmark tool to specific implementation
- Confusing to have diskann benchmarks in vec repo

**Status:** Rejected - standalone is cleaner

### Option 3: Minimal Deprecation

**Just deprecate without preserving anything:**

**Pros:**
- Minimal effort

**Cons:**
- Loses valuable benchmark infrastructure
- Loses experiment methodology
- Wastes months of research findings

**Status:** Rejected - too much value left on table

## Tasks

### Phase 1: Create node-vector-bench repo
- [ ] Create new GitHub repo: `node-vector-bench`
- [ ] Initialize with README explaining purpose
- [ ] Set up package.json structure
- [ ] Define runner interface

### Phase 2: Migrate benchmark infrastructure
- [ ] Copy benchmark harness (`benchmarks/src/`)
- [ ] Copy USearch runner
- [ ] Copy benchmark profiles
- [ ] Copy dataset utilities
- [ ] Update imports for new structure
- [ ] Test with USearch runner

### Phase 3: Add sqlite-vec runner
- [ ] Copy sqlite-vec runner from sqlite-diskann
- [ ] Test sqlite-vec runner in new repo
- [ ] Verify benchmarks work end-to-end

### Phase 4: Migrate experiment methodology
- [ ] Copy experiment template and README
- [ ] Update paths in experiment docs
- [ ] Link to node-vector-bench from experiments

### Phase 5: Deprecate sqlite-diskann npm package
- [ ] Update package.json to v0.2.0
- [ ] Add `"deprecated"` field with message
- [ ] Update README with deprecation banner
- [ ] Add post-mortem narrative to README
- [ ] Publish final version to npm

### Phase 6: Archive sqlite-diskann repo
- [ ] Add deprecation notice to README
- [ ] Link to node-vector-bench
- [ ] Link to post-mortem section
- [ ] Update repo description
- [ ] Archive repository on GitHub

**Verification:**

```bash
# Test benchmark harness in sqlite-vec
cd ../sqlite-vec/benchmarks
npm install
npm run bench -- profiles/quick.json

# Verify npm deprecation
npm view @photostructure/sqlite-diskann

# Check archived repo status
# (Manual GitHub UI check)
```

## Post-mortem narrative

Draft for README deprecation notice and standalone document.

### Why we abandoned sqlite-diskann

We extracted the DiskANN implementation from libSQL into a standalone SQLite extension (Feb 9-10). The code worked, but benchmarks were bad. We spent the next week figuring out why and trying to fix it.

**The recall problem.** At 10k vectors, recall was 97%. At 100k, it dropped to 0-1%. We tuned `searchListSize`, `pruning_alpha`, and `MIN_DEGREE` with no effect. Turns out the default 4KB block size only fits 2-3 edges per node for 256D vectors (`(4096 - 1040) / 1040 = 2.9`). The graph fragmented into disconnected components. See `_done/20260210-diskann-recall-fix-investigation.md`.

**Fixing recall broke everything else.** Switching to auto-calculated 40KB blocks (Feb 11) restored recall to 98%. But 40KB per node means 7.5 GB indexes for 100k vectors (sqlite-vec uses 100 MB). Build time: 3626 seconds vs 1.6 seconds. See `experiment-005-100k-recall.md`.

**We tried to optimize build performance.** None of it worked well enough:

- **Parallel construction** (`_todo/20260210-parallel-graph-construction.md`): SQLite only allows one writer. Graph mutations can't be parallelized.
- **BLOB caching** (`_todo/20260211-build-speed-optimization.md`): LRU cache with refcounting. 37% faster (707s to 445s for 25k). Still 1,667x slower than brute force.
- **Batch insert API** (`_todo/20260211-serial-batch-insert.md`): Persistent cache across inserts, amortized SAVEPOINT overhead. 10-20% gain. We stopped benchmarking at that point.
- **Lazy back-edges** (`_todo/20260212-lazy-back-edges.md`): Defer edge updates, batch repair at commit. Helped throughput but hurt recall at small scale. Complex code for marginal gains.
- **Parameter sweeps** (`_done/20260212-max-neighbors-sweep.md`, experiments 002b-004): Tested `max_neighbors` from 24 to 64 and `searchListSize` from 100 to 500. Recall improved, build time didn't.

**The problem is architectural.** DiskANN assumes direct memory control: mmap the graph, traverse it cheaply. SQLite's BLOB I/O adds a 40KB read per graph hop. Each insert touches ~200 nodes, so that's 16 MB of I/O per insert. At 100k inserts, the cumulative BLOB I/O is around 800 TB. No amount of caching or batching fixes that.

**Final numbers (Feb 15):**

- Build: 3626s vs 1.6s (sqlite-vec), 2,268x slower
- Index: 7.5 GB vs 100 MB, 74x larger
- QPS: ~46, same as sqlite-vec brute force at 100k
- Recall: 98% (the one metric that worked)

**Use `@photostructure/sqlite-vec` instead.** For datasets under 100k, brute force builds in 1.6 seconds with perfect recall. For datasets over 1M, use an external index like USearch with mmap.

See also: `_todo/` and `_done/` directories for full TPPs, `experiments/` for benchmark data.

## Notes

**Session 2026-02-15:**
- User showed benchmark results: diskann loses on all metrics except recall
- Build: 3626s vs 1.6s (sqlite-vec)
- Index: 7.5GB vs 100MB (sqlite-vec)
- QPS: ~46 (both libraries similar)
- User requested help with deprecation and migration
- Decision: Create standalone `node-vector-bench` repo (not migrate to sqlite-vec)
- Reason: sqlite-vec has own benchmarks, standalone benefits whole ecosystem
- Wrote post-mortem narrative summarizing optimization journey
- Catalogued TPPs showing what was tried and why it failed
- Catalogued assets: benchmark harness is valuable, experiment methodology is valuable
- sqlite-vec has minimal benchmark infrastructure currently
