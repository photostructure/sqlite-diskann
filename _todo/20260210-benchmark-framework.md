# Benchmark Framework for sqlite-diskann vs sqlite-vec

## Summary

Comprehensive benchmark framework comparing sqlite-diskann (DiskANN approximate search) against sqlite-vec (brute-force exact search). Framework follows industry standards from ann-benchmarks with recall@k metrics, QPS measurements, and configurable profiles.

**Status:** Framework complete and tested with sqlite-vec. DiskANN extension loading resolved. Ready for final benchmark runs and validation.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [x] Cleanup & Documentation
- [ ] Final Review (run benchmarks and validate results)

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `benchmarks/README.md` - Comprehensive usage guide
- [ann-benchmarks](https://ann-benchmarks.com/) - Industry standards
- [ANN-Benchmarks datasets.py](https://github.com/erikbern/ann-benchmarks/blob/main/ann_benchmarks/datasets.py)

## Description

**Problem:** Need fair, comprehensive benchmark comparing sqlite-diskann to sqlite-vec demonstrating performance characteristics and trade-offs.

**Constraints:**

- Must complete within 10-30 minutes (configurable profiles)
- Must use industry-standard metrics (recall@k, QPS, latency percentiles)
- Must support multiple vector dimensions (64d to 512d)
- Must test realistic dataset sizes (10k to 1M vectors)

**Success Criteria:**

- ✅ Framework produces clear console table output
- ✅ Demonstrates comparative results with recall@k metrics
- ✅ Includes quick (<2min), standard (10-15min), recall-sweep profiles
- ✅ Results exportable to JSON and Markdown
- ✅ Extension loading resolved (see `_done/20260210-extension-loading-fix.md`)
- ⏳ Validate DiskANN benchmarks produce expected recall@k and performance
- ⏳ Document benchmark results in TPP

## Tribal Knowledge

### Framework Implementation

**Binary dataset format:**
- Magic header "VECDATA\0" + count + dim + Float32Array
- Seeded RNG (seed=42) provides reproducible datasets
- 4 datasets generated: small (64d×10k), medium (256d×100k), large (512d×100k), xlarge (512d×1M)
- Sizes: 2.4 MB to 195 MB

**Ground truth computation:**
- Uses sqlite-vec brute-force exact k-NN as reference
- Cached to JSON for reuse
- Recall@k metric: `|predicted ∩ ground_truth| / k`

**Verified with sqlite-vec:**
- QPS: ~2,253 queries/second on small-64d-10k
- Latency p50: 0.43ms, p95: 0.50ms
- Recall: 100% (exact search, as expected)
- Build time: 0.1s, Index: 2.7 MB

**Extension loading:**
- DiskANN extension loads successfully via `diskann_sqlite.h` conditional header approach
- See `_done/20260210-extension-loading-fix.md` for technical details
- All 175 C tests pass (126 C API + 49 vtab)

### sqlite-vec API Quirks

1. **Auto-generated rowid:** Must let SQLite auto-generate (1-based), then subtract 1 when reading
2. **k parameter:** Use `WHERE embedding MATCH ? AND k = ?` (don't combine with LIMIT)
3. **Extension loading:** Requires both `allowExtension: true` and `db.enableLoadExtension(true)`

### Design Decisions

**Synthetic datasets:** Faster to generate, configurable dimensions, reproducible (seeded RNG). Optional: add SIFT/GIST later.

**Separate runners:** Abstract `BenchmarkRunner` interface allows comparing different libraries (vec uses raw SQL, diskann uses TypeScript API).

**Three profiles:**
- `quick.json`: <2 min smoke test (CI/development)
- `standard.json`: 10-15 min comprehensive benchmark
- `recall-sweep.json`: 15-20 min parameter tuning

### Expected Performance (from ann-benchmarks research)

- Small (10k × 64d): DiskANN ~10x faster, 95-99% recall
- Medium (100k × 256d): DiskANN ~160x faster, 95-99% recall
- Large (1M × 512d): DiskANN ~200x+ faster, 95-99% recall

**Trade-off:** DiskANN sacrifices 1-5% recall for 10-200x speedup.

## Remaining Work

1. Run DiskANN benchmarks: `cd benchmarks && npm run bench:quick`
2. Validate recall@k calculations match expected 95-99% range
3. Run full benchmark suite (`bench:standard`, `bench:recall`)
4. Document results in this TPP
5. Move TPP to `_done/` when complete

## Tasks Completed

**Framework (~2,000 lines TypeScript):**
- ✅ Core types, config, dataset generation with binary I/O
- ✅ Ground truth computation and caching
- ✅ Abstract BenchmarkRunner interface
- ✅ diskann-runner and vec-runner implementations
- ✅ Metrics (recall@k, QPS, latency percentiles)
- ✅ Console, JSON, and Markdown reporters
- ✅ Three profiles (quick/standard/recall-sweep)
- ✅ CLI scripts (prepare-datasets, run-benchmark)
- ✅ Comprehensive documentation (README, KNOWN_ISSUES)

**Extension:**
- ✅ DiskANN extension builds and loads successfully
- ✅ All 175 C tests pass (126 C API + 49 vtab)
- ✅ Extension loading resolved via `diskann_sqlite.h`

**Verification:**
- ✅ sqlite-vec benchmarks working perfectly (~2,253 QPS, 100% recall)
- ✅ All 4 datasets generated (2.4 MB to 195 MB)

## Tasks Remaining

- [ ] Run DiskANN benchmarks: `cd benchmarks && npm run bench:quick`
- [ ] Validate recall@k matches expected 95-99% range
- [ ] Run full suite: `npm run bench:standard` and `npm run bench:recall`
- [ ] Document results in TPP
- [ ] Optional: Add SIFT/GIST dataset support

## Critical Files

**Framework (~2,000 lines TypeScript):**
- `benchmarks/src/harness.ts` - Main orchestration
- `benchmarks/src/runners/{diskann,vec}-runner.ts` - Library implementations
- `benchmarks/src/dataset.ts` - Binary I/O and generation
- `benchmarks/src/ground-truth.ts` - Exact k-NN via brute force
- `benchmarks/src/metrics.ts` - Recall@k calculations
- `benchmarks/src/reporters/` - Console, JSON, Markdown output

**Configuration:**
- `benchmarks/profiles/{quick,standard,recall-sweep}.json`
- `benchmarks/package.json` - Using node:sqlite

**Documentation:**
- `benchmarks/README.md` - Complete usage guide
- `benchmarks/KNOWN_ISSUES.md` - Extension loading notes

## Notes

**Industry research sources:**
- [ANN-Benchmarks](https://ann-benchmarks.com/) - Standard benchmarking methodology
- [ann-benchmarks datasets.py](https://github.com/erikbern/ann-benchmarks/blob/main/ann_benchmarks/datasets.py) - Dataset format reference
- [Understanding ANN Benchmarks](https://zilliz.com/glossary/ann-benchmarks) - Metrics explanation
- [HNSW vs DiskANN](https://www.vectroid.com/resources/HNSW-vs-DiskANN-comparing-the-leading-ANN-algorithm) - Algorithm comparison

**Framework status:** Production-ready. Extension loading resolved (see `_done/20260210-extension-loading-fix.md`). Ready for final benchmark runs.
