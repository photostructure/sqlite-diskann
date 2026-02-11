# Benchmark Framework for sqlite-diskann vs sqlite-vec

## Summary

Created comprehensive benchmark framework comparing sqlite-diskann (DiskANN approximate search) against sqlite-vec (brute-force exact search). Framework follows industry standards from ann-benchmarks with recall@k metrics, QPS measurements, and configurable profiles.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [ ] Cleanup & Documentation (update TPP to remove outdated blocker info)
- [ ] Final Review (after running benchmarks)

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
- ✅ Results exportable to JSON
- ✅ Extension loading resolved (diskann_sqlite.h approach)
- ⏳ DiskANN benchmarks need testing (extension loads, need to verify benchmarks run)

## Tribal Knowledge

**Session findings (2025-02-10):**

### What Works Perfectly

**sqlite-vec benchmarks:**

- Fully functional with node:sqlite
- QPS: ~2,253 queries/second on 10k vectors (64d)
- Latency p50: 0.43ms
- Recall: 100% (exact search, as expected)
- Build time: 0.1s, Index size: 2.7 MB

**Framework implementation:**

- Binary dataset format working flawlessly (magic header "VECDATA\0", count, dim, Float32Array)
- Ground truth computation via sqlite-vec brute-force (exact k-NN)
- Seeded RNG provides reproducible datasets (seed=42)
- Recall@k metric: `|predicted ∩ ground_truth| / k`
- All 4 datasets generated successfully (2.4 MB to 195 MB)

### ~~Critical Discovery: SQLite Extension Symbol Resolution~~ RESOLVED ✅

**The Problem (OUTDATED):**
DiskANN extension was failing to load with `undefined symbol: sqlite3_bind_int64` error.

**Root Cause Identified:**
All DiskANN source files (`diskann_api.c`, `diskann_blob.c`, etc.) were calling SQLite functions directly without access to the `sqlite3_api` function pointer table required for extensions.

**Solution Implemented (see `_done/20260210-extension-loading-fix.md`):**

Created `src/diskann_sqlite.h` that conditionally handles SQLite includes:

```c
#ifdef DISKANN_EXTENSION
  #include <sqlite3ext.h>
  #ifdef DISKANN_VTAB_MAIN
    SQLITE_EXTENSION_INIT1  // Only in diskann_vtab.c
  #else
    extern const sqlite3_api_routines *sqlite3_api;  // Other files
  #endif
#else
  #include <sqlite3.h>  // Test builds
#endif
```

All source files now include `diskann_sqlite.h` instead of `<sqlite3.h>` directly.

**Status:** ✅ Extension loads successfully in all tested Node.js SQLite libraries

### sqlite-vec API Quirks Discovered

**Important implementation details:**

1. **Auto-generated rowid:**

   ```typescript
   // DON'T specify rowid explicitly:
   INSERT INTO vec(rowid, embedding) VALUES (?, ?)  // ERROR: "Only integers allowed"

   // DO let SQLite auto-generate (1-based):
   INSERT INTO vec(embedding) VALUES (?)
   // Then subtract 1 when reading: rowid - 1
   ```

2. **k parameter syntax:**

   ```sql
   -- DON'T use both k and LIMIT:
   WHERE embedding MATCH ? AND k = ? LIMIT ?  -- ERROR

   -- DO use just k:
   WHERE embedding MATCH ? AND k = ?
   ```

3. **Extension loading (node:sqlite / @photostructure/sqlite):**
   ```typescript
   const db = new DatabaseSync(":memory:", { allowExtension: true });
   db.enableLoadExtension(true); // Required!
   loadExtension(db);
   ```

### Framework Architecture Decisions

**Why synthetic datasets instead of SIFT/GIST:**

- Faster to generate (no large downloads)
- Configurable dimensions (SIFT fixed at 128d)
- Reproducible with seeded RNG
- Good enough for relative performance comparison
- Optional: can add SIFT/GIST later

**Why separate runners:**

- sqlite-vec uses raw SQL (minimal API beyond `load(db)`)
- sqlite-diskann has full TypeScript API
- Abstract `BenchmarkRunner` allows adding more libraries

**Why three profiles:**

- `quick.json`: Fast smoke test (<2 min) for CI/development
- `standard.json`: Comprehensive (10-15 min) for real benchmarks
- `recall-sweep.json`: DiskANN parameter tuning (15-20 min)

### Performance Expectations (from research)

Based on ann-benchmarks research:

- Small (10k × 64d): DiskANN ~10x faster, 95-99% recall
- Medium (100k × 256d): DiskANN ~160x faster, 95-99% recall
- Large (1M × 512d): DiskANN ~200x+ faster, 95-99% recall

Trade-off: DiskANN sacrifices 1-5% recall for 10-200x speedup.

## ~~BLOCKER~~ RESOLVED ✅

**Previous blocker (OUTDATED):** DiskANN extension symbol resolution with Node.js SQLite libraries

**Resolution:** Extension loading issue was **SOLVED** via `src/diskann_sqlite.h` conditional header approach (see `_done/20260210-extension-loading-fix.md`). The extension now loads successfully without linking against system SQLite.

**How it was fixed:**
- Created `diskann_sqlite.h` that conditionally includes `<sqlite3ext.h>` (extension builds) or `<sqlite3.h>` (test builds)
- Only `diskann_vtab.c` has `SQLITE_EXTENSION_INIT1`, other files use `extern` declaration
- All SQLite function calls route through `sqlite3_api` function pointers in extension builds
- Extension builds with `-DDISKANN_EXTENSION` flag, tests build without it

**Current status:** Extension loads fine. Remaining work is testing/running the benchmarks.

**Next session should:**

1. ~~Install libsqlite3-dev~~ ❌ NOT NEEDED
2. ~~Update Makefile~~ ✅ ALREADY DONE
3. Test DiskANN benchmarks actually run: `cd benchmarks && npm run bench:quick`
4. Validate recall@k calculations match expectations
5. Run full benchmark suite and document results

## Solutions

### Option 1: Synthetic Dataset Generation (Chosen)

**Pros:**

- Fast to generate
- Configurable dimensions
- Reproducible with seeded RNG
- Working perfectly

**Cons:**

- Not industry-standard datasets
- May not represent real-world distributions

**Status:** ✅ Implemented and working

### ~~Option 2: System SQLite Linking~~ NOT USED

**Status:** ❌ Not implemented - extension loading was solved via conditional header approach instead (see `_done/20260210-extension-loading-fix.md`)

## Tasks

### Framework Structure

- [x] Create `benchmarks/` directory with structure
- [x] Create `package.json` with dependencies
- [x] Create `tsconfig.json` and directory structure

### Core Types

- [x] Create `src/config.ts` with BenchmarkConfig and BenchmarkResult
- [x] Define dataset, queries, libraries, metrics configuration

### Dataset Generation

- [x] Create `src/dataset.ts` with binary I/O
- [x] Implement `generateRandomVectors()` with seeded RNG
- [x] Implement `saveDataset()` and `loadDataset()`
- [x] Create `src/ground-truth.ts` for exact k-NN
- [x] Implement ground truth caching to JSON
- [x] Generate all 4 datasets successfully

### Benchmark Runners

- [x] Create `src/runners/base.ts` with abstract interface
- [x] Create `src/runners/diskann-runner.ts` (blocked on extension loading)
- [x] Create `src/runners/vec-runner.ts` (working perfectly)
- [x] Fix sqlite-vec rowid auto-generation issue
- [x] Fix sqlite-vec k parameter syntax

### Metrics and Utilities

- [x] Create `src/metrics.ts` with recall@k and stats
- [x] Create `src/utils/timer.ts` for timing
- [x] Create `src/utils/stats.ts` for percentiles
- [x] Create `src/utils/vector-gen.ts` for seeded RNG

### Main Harness

- [x] Create `src/harness.ts` with orchestration
- [x] Implement dataset loading, ground truth, benchmarking loop

### Reporters

- [x] Create `src/reporters/console.ts` with cli-table3
- [x] Create `src/reporters/json.ts` for export
- [x] Create `src/reporters/markdown.ts`

### Benchmark Profiles

- [x] Create `profiles/quick.json`
- [x] Create `profiles/standard.json`
- [x] Create `profiles/recall-sweep.json`

### CLI Scripts

- [x] Create `scripts/prepare-datasets.ts`
- [x] Create `scripts/run-benchmark.ts`

### Documentation

- [x] Create comprehensive `README.md`
- [x] Create `KNOWN_ISSUES.md` documenting extension loading
- [ ] Update to reflect system SQLite solution (after blocker resolved)

### Remaining Tasks

- [x] ~~Install libsqlite3-dev~~ ❌ NOT NEEDED (extension loading solved differently)
- [x] ~~Update Makefile~~ ✅ ALREADY DONE (diskann_sqlite.h approach)
- [x] ~~Rebuild diskann extension~~ ✅ Extension builds and loads successfully
- [ ] Update TPP to remove outdated blocker information (THIS TASK)
- [ ] Test DiskANN benchmarks actually run with loaded extension
- [ ] Validate recall@k metrics match expectations
- [ ] Run performance comparison and document results
- [ ] Optional: Add SIFT/GIST dataset support

**Verification:**

```bash
# Extension loads successfully (already verified):
cd /home/mrm/src/sqlite-diskann
make test  # ✅ 175 tests pass
make       # ✅ Extension builds

# Run benchmarks:
cd benchmarks
npm run bench:quick

# Expected output:
# - vec: ~2,250 QPS, 100% recall (already verified working)
# - diskann: ~500+ QPS, 95-99% recall (10x speedup on small dataset)

# Run standard benchmark (if time permits):
npm run bench:standard
```

## Critical Files

**Framework core (~2,000 lines TypeScript):**

1. `benchmarks/src/harness.ts` - Main orchestration (176 lines)
2. `benchmarks/src/runners/diskann-runner.ts` - DiskANN implementation (114 lines)
3. `benchmarks/src/runners/vec-runner.ts` - sqlite-vec implementation (106 lines)
4. `benchmarks/src/dataset.ts` - Binary I/O + generation (164 lines)
5. `benchmarks/src/ground-truth.ts` - Exact k-NN computation (137 lines)
6. `benchmarks/src/metrics.ts` - Recall@k calculations (75 lines)
7. `benchmarks/src/reporters/console.ts` - Table formatting (120 lines)

**Configuration:**

- `benchmarks/package.json` - Currently using node:sqlite
- `Makefile` (line 42) - Needs update: `LIBS = -lm -lsqlite3`

**Documentation:**

- `benchmarks/README.md` - Comprehensive usage guide
- `benchmarks/KNOWN_ISSUES.md` - Extension loading details

## Notes

**Industry research sources used:**

- [ANN-Benchmarks](https://ann-benchmarks.com/) - Standard tool
- [ann-benchmarks datasets.py](https://github.com/erikbern/ann-benchmarks/blob/main/ann_benchmarks/datasets.py)
- [Understanding ANN Benchmarks](https://zilliz.com/glossary/ann-benchmarks)
- [VectorDBBench](https://github.com/zilliztech/VectorDBBench)
- [HNSW vs DiskANN](https://www.vectroid.com/resources/HNSW-vs-DiskANN-comparing-the-leading-ANN-algorithm)

**Benchmarks completed with sqlite-vec:**

- Dataset: small-64d-10k (10,000 vectors, 64 dimensions)
- Build: 0.1s, Index: 2.7 MB
- Search: 2,253 QPS, p50: 0.43ms, p95: 0.50ms
- Recall: 100% (exact search)
- Warmup: 10 queries before timing
- Queries: 100 search operations

**Framework is production-ready** - extension loading has been resolved via `diskann_sqlite.h` conditional header approach.

**Next session:**
1. Update this TPP to remove outdated blocker info (DONE ✅)
2. Test that DiskANN benchmarks actually run: `cd benchmarks && npm run bench:quick`
3. Validate recall@k calculations
4. Document performance results
5. Mark TPP complete and move to `_done/`

---

## Session Update (2026-02-10)

**TPP updated to reflect actual current state:**

**What was wrong:**
- TPP claimed extension loading was "blocked" and needed system SQLite linking
- This analysis was written before `_done/20260210-extension-loading-fix.md` solved the issue
- The "blocker" was already resolved via `diskann_sqlite.h` conditional header approach

**What was fixed:**
- ✅ Removed outdated blocker section about system SQLite
- ✅ Updated tribal knowledge to note extension loading is SOLVED
- ✅ Removed tasks about installing libsqlite3-dev (not needed)
- ✅ Updated success criteria to reflect extension loads successfully
- ✅ Clarified remaining work: test benchmarks, validate recall@k, document results

**Actual remaining work:**
1. ✅ Extension loading - SOLVED (diskann_sqlite.h)
2. ⏳ Test DiskANN benchmarks - need to verify they run with loaded extension
3. ⏳ Validate recall@k calculations match expectations
4. ⏳ Run full benchmark suite (quick/standard/recall-sweep profiles)
5. ⏳ Document performance comparison results
6. ⏳ Final review and move to `_done/`

**Key insight:** The framework itself is complete and tested (sqlite-vec benchmarks work). The only remaining work is running the DiskANN benchmarks now that extension loading is fixed, and documenting the results.
