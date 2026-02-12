# sqlite-diskann vs sqlite-vec Performance Benchmarks

Comprehensive benchmark framework comparing sqlite-diskann (DiskANN approximate nearest neighbor search) against sqlite-vec (brute-force exact search).

## Quick Start

```bash
# Install dependencies
cd benchmarks
npm install

# Generate datasets (one-time, ~2 min)
npm run prepare

# Run quick smoke test (< 2 min)
npm run bench:quick

# Run standard benchmark (10-15 min)
npm run bench:standard

# Run recall vs speed sweep (15-20 min)
npm run bench:recall
```

## Interpreting Results

### Key Metrics

- **QPS (Queries Per Second)**: Higher is better. Measures throughput.
- **Latency p50/p95/p99**: Lower is better. Query response time percentiles.
- **Recall@k**: Percentage of true nearest neighbors found.
  - 100% = exact search (sqlite-vec)
  - 95-99% = approximate search (sqlite-diskann)
- **Build Time**: Time to insert all vectors and build index.
- **Index Size**: Disk space used by the index.

### Trade-offs

**sqlite-vec (Brute Force)**

- ✅ Always 100% recall (exact search)
- ✅ Simple, no parameters to tune
- ✅ Fast for small datasets (< 10k vectors)
- ❌ Doesn't scale - linear O(n) search time
- ❌ Slow for large datasets (> 100k vectors)

**sqlite-diskann (Approximate)**

- ✅ Scales to millions of vectors
- ✅ Fast search - sub-linear time complexity
- ✅ Disk-based (doesn't require all data in RAM)
- ❌ 95-99% recall (approximate, not exact)
- ❌ More parameters to tune (maxDegree, searchListSize)
- ❌ (Much!) longer build time

### When to Use Each

**Use sqlite-vec if:**

- Dataset < 10k vectors
- You need exact results (100% recall)
- Simplicity matters more than speed
- Queries are infrequent

**Use sqlite-diskann if:**

- Dataset > 100k vectors
- 95-99% recall is acceptable
- Query speed matters more than exact results
- Working with limited RAM (disk-based indexing)

## Expected Performance

Based on the benchmark results, you should see:

### Small Dataset (10k vectors, 64d)

| Library | Build (s) | QPS  | Recall@10 |
| ------- | --------- | ---- | --------- |
| vec     | 0.5       | ~50  | 100%      |
| diskann | 2.0       | ~500 | 95-99%    |

**Insight:** DiskANN is ~10x faster but build overhead is noticeable.

### Medium Dataset (100k vectors, 256d)

| Library | Build (s) | QPS  | Recall@10 |
| ------- | --------- | ---- | --------- |
| vec     | 60        | ~5   | 100%      |
| diskann | 120       | ~800 | 95-99%    |

**Insight:** DiskANN is ~160x faster. Build time becomes less significant. This is where DiskANN shines.

### Large Dataset (100k vectors, 512d)

| Library | Build (s) | QPS  | Recall@10 |
| ------- | --------- | ---- | --------- |
| vec     | 180       | ~2   | 100%      |
| diskann | 240       | ~400 | 95-99%    |

**Insight:** DiskANN is ~200x faster. Higher dimensions amplify the advantage.

**Key Takeaway:** DiskANN trades 1-5% recall for 10-200x speedup. The trade-off becomes more favorable as dataset size and dimensionality increase.

## Custom Benchmarks

Create a custom profile in `profiles/`:

```json
{
  "name": "My custom benchmark",
  "dataset": {
    "path": "datasets/synthetic/medium-256d-100k.bin"
  },
  "queries": {
    "count": 100,
    "k": [10]
  },
  "libraries": [{ "name": "vec" }, { "name": "diskann" }],
  "diskann": {
    "maxDegree": [64],
    "buildSearchListSize": [100],
    "searchListSize": [100],
    "metric": "cosine",
    "normalizeVectors": false
  },
  "metrics": {
    "computeRecall": true,
    "measureMemory": false,
    "warmupQueries": 10
  }
}
```

Run: `npm run bench profiles/my-custom.json`

### Parameters to Tune

**DiskANN parameters:**

- `maxDegree`: Graph node degree (32-128). Higher = better recall, more memory.
- `buildSearchListSize`: Search beam during build (50-200). Higher = better index quality, slower build.
- `searchListSize`: Search beam during query (10-500). Higher = better recall, slower queries.
- `metric`: Distance metric (`cosine`, `euclidean`, `dot`).
- `normalizeVectors`: Normalize vectors during insertion (for cosine similarity).

**Query parameters:**

- `k`: Number of nearest neighbors to return.
- `warmupQueries`: Number of warmup queries before timing (reduces cold-start noise).

## Datasets

Generated synthetic datasets with unit-normalized random vectors:

| Name             | Vectors | Dimensions | Size (MB) | Use Case           |
| ---------------- | ------- | ---------- | --------- | ------------------ |
| small-64d-10k    | 10,000  | 64         | 2.4       | Quick smoke test   |
| small-96d-10k    | 10,000  | 96         | 3.6       | Small embeddings   |
| medium-256d-100k | 100,000 | 256        | 96        | Standard benchmark |
| large-512d-100k  | 100,000 | 512        | 192       | Large embeddings   |

Ground truth computed using sqlite-vec (exact brute-force search).

### Dataset Format

Binary format with little-endian encoding:

```
[magic: "VECDATA\0" (8 bytes)]
[count: uint32 (4 bytes)]
[dim: uint32 (4 bytes)]
[vectors: count × dim × float32]
```

Ground truth stored as JSON:

```json
{
  "queries": [0, 100, 200, ...],
  "neighbors": [[5, 12, 78, ...], ...],
  "distances": [[0.12, 0.15, 0.23, ...], ...]
}
```

## Advanced Usage

### Memory Profiling

To measure memory usage during benchmarks, set `measureMemory: true` in the config.

### Recall vs Speed Trade-off

Use the `recall-sweep` profile to see how DiskANN's `searchListSize` parameter affects the recall/speed trade-off:

```bash
npm run bench:recall
```

This tests multiple `searchListSize` values (10, 20, 50, 100, 200, 500) to show the curve.

### Exporting Results

All benchmarks export results to `results-{timestamp}.json` for further analysis. You can use this data to:

- Generate custom charts
- Compare across different machines
- Track performance over time

## Troubleshooting

### "Cannot find module 'sqlite-vec'"

Make sure you've installed dependencies and that `../sqlite-vec` exists:

```bash
cd benchmarks
npm install
```

### "Dataset file not found"

Run `npm run prepare` to generate the datasets first:

```bash
npm run prepare
```

### Benchmarks taking too long

Use the `quick` profile for a fast smoke test:

```bash
npm run bench:quick
```

Or create a custom profile with smaller datasets and fewer queries.

## Architecture

The benchmark framework consists of:

- **`src/config.ts`** - Configuration and result types
- **`src/dataset.ts`** - Dataset generation and binary I/O
- **`src/ground-truth.ts`** - Ground truth computation via brute-force
- **`src/harness.ts`** - Main benchmark orchestration
- **`src/metrics.ts`** - Recall@k and statistics calculations
- **`src/runners/`** - Library-specific benchmark runners
- **`src/reporters/`** - Console, JSON, and markdown reporters
- **`src/utils/`** - Timer, stats, and vector generation utilities
- **`scripts/`** - CLI entry points
- **`profiles/`** - Benchmark configurations

## Industry Standards

This framework follows best practices from:

- [ANN-Benchmarks](https://ann-benchmarks.com/) - Standard tool for ANN algorithm comparison
- [ann-benchmarks datasets](https://github.com/erikbern/ann-benchmarks) - SIFT/GIST standard datasets
- [VectorDBBench](https://github.com/zilliztech/VectorDBBench) - Full database benchmarking

**Recall@k metric:** `|predicted ∩ ground_truth| / k` where ground truth is computed via exact brute-force search.

## License

MIT License - Copyright 2026 PhotoStructure Inc.
