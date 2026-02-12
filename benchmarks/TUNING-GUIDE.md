# Parameter Tuning Guide

## Goal: Find Optimal Defaults for 100k-500k+ Vectors

This guide explains how to systematically tune DiskANN parameters for large-scale deployments.

## Quick Start

```bash
# Run all tuning benchmarks (takes ~2-4 hours)
npm run tune:all

# Or run individual sweeps:
npm run tune:insert-list    # ~30 min
npm run tune:max-neighbors  # ~30 min
npm run tune:scaling        # ~60-90 min
```

## Benchmark Profiles

### 1. Insert List Size Sweep (`param-sweep-insert-list.json`)

**What:** Tests insert_list_size = [50, 75, 100, 150, 200]

**Measures:**

- Build time (expect linear relationship)
- Recall@k (should plateau around 100-150)
- Index size (minimal impact)

**Expected results:**

```
insert_list_size  Build Time  Recall@10  Index Size
50                ~110s       91-93%     ~2GB
75                ~165s       94-96%     ~2GB
100               ~220s       95-97%     ~2GB
150               ~330s       96-98%     ~2GB
200               ~440s       97-98%     ~2GB
```

**Sweet spot:** Where recall curve flattens (likely 75-100)

### 2. Max Neighbors Sweep (`param-sweep-max-neighbors.json`)

**What:** Tests max_neighbors = [24, 32, 48, 64]

**Measures:**

- Block size calculation
- Index size (major impact!)
- Recall@k
- Build time

**Expected results:**

```
max_neighbors  Block Size  Index Size  Recall@10  Build Time
24             28KB        1.4GB       93-95%     ~200s
32             40KB        2.0GB       95-97%     ~220s
48             60KB        3.0GB       96-98%     ~240s
64             80KB        4.0GB       97-99%     ~260s
```

**Sweet spot:** Balance index size vs recall (likely 24-32)

### 3. Scaling Test (`scaling-test.json`)

**What:** Tests [10k, 25k, 50k, 100k, 200k] vectors

**Measures:**

- Build time scaling (should be O(n log n))
- Query time scaling (should be O(log n))
- Recall degradation at scale
- When DiskANN becomes faster than brute-force

**Expected results:**

```
Vectors  Build Time  QPS (DiskANN)  QPS (vec)  Recall@10  Crossover
10k      ~40s        150            250        98%        ❌ vec wins
25k      ~120s       120            200        97%        ❌ vec wins
50k      ~280s       100            150        96%        ⚖️  close
100k     ~640s       90             80         95%        ✅ diskann wins
200k     ~1500s      85             40         94%        ✅ diskann wins
```

**Sweet spot:** 100k+ is where DiskANN pays off

## Interpreting Results

### Build Time

**Linear with insert_list_size:**

```
build_time ≈ num_vectors × insert_list_size × block_read_time
```

**Example:**

- 50k vectors × 100 searches × 0.05ms = 250s
- 50k vectors × 200 searches × 0.05ms = 500s

**Target:** <500s for 100k vectors (acceptable for batch indexing)

### Recall@k

**Diminishing returns:**

```
insert_list_size  Recall@10
50                91%
75                94%
100               96%
150               97%
200               97.5%
```

Notice: 50→100 gives +5%, but 100→200 gives only +1.5%

**Target:** ≥95% recall@10 (industry standard)

### Index Size

**Dominated by block_size:**

```
index_size ≈ num_vectors × block_size
block_size = f(dimensions, max_neighbors)
```

**For 256D:**

```
max_neighbors  block_size  100k index
24             28KB        2.8GB
32             40KB        4.0GB
48             60KB        6.0GB
64             80KB        8.0GB
```

**Target:** <5GB for 100k vectors (reasonable for production)

### Query Performance

**Should scale logarithmically:**

```
Vectors   Expected QPS
10k       ~150
50k       ~120
100k      ~100
500k      ~80
1M        ~70
```

**Target:** >80 QPS @ 100k vectors

## Recommended Defaults from Analysis

Based on expected tuning results, here are proposed new defaults:

### Conservative (High Recall, Slower Builds)

```typescript
{
  maxDegree: 48,              // Good connectivity
  buildSearchListSize: 150,   // High quality graph
  searchListSize: 200,        // High recall searches
  pruning_alpha: 1.4
}
```

- Build: ~330s/50k
- Recall@10: 96-98%
- Index: ~3GB/100k
- Use case: Production with high recall requirements

### Balanced (Recommended)

```typescript
{
  maxDegree: 32,              // Reasonable index size
  buildSearchListSize: 100,   // Fast builds
  searchListSize: 150,        // Good recall
  pruning_alpha: 1.4
}
```

- Build: ~220s/50k
- Recall@10: 95-97%
- Index: ~2GB/100k
- Use case: General production use

### Fast (Quick Builds, Lower Recall)

```typescript
{
  maxDegree: 24,              // Smaller index
  buildSearchListSize: 75,    // Very fast builds
  searchListSize: 100,        // Acceptable recall
  pruning_alpha: 1.4
}
```

- Build: ~110s/50k
- Recall@10: 91-94%
- Index: ~1.4GB/100k
- Use case: Development, CI/CD, or low-recall-ok applications

## Running Parameter Sweeps

### Manual Sweep

```bash
# Test different insert_list_size values
for size in 50 75 100 150 200; do
  echo "Testing insert_list_size=$size"

  # Create custom profile
  cat > /tmp/test-$size.json <<EOF
{
  "name": "test-insert-$size",
  "datasets": [{"name": "synthetic", "dimensions": 256, "count": 50000}],
  "runners": [{
    "type": "diskann",
    "config": {"buildSearchListSize": $size}
  }]
}
EOF

  # Run benchmark
  npm run bench -- --profile=/tmp/test-$size.json
done
```

### Automated Sweep (Recommended)

```bash
# Add to package.json scripts:
"tune:insert-list": "npm run bench -- --profile=profiles/param-sweep-insert-list.json",
"tune:max-neighbors": "npm run bench -- --profile=profiles/param-sweep-max-neighbors.json",
"tune:scaling": "npm run bench -- --profile=profiles/scaling-test.json",
"tune:all": "npm run tune:insert-list && npm run tune:max-neighbors && npm run tune:scaling"
```

Then run:

```bash
npm run tune:all > tuning-results.txt 2>&1
```

## Analyzing Results

### 1. Plot Build Time vs Insert List Size

```python
import json
import matplotlib.pyplot as plt

# Load results
with open('results-*.json') as f:
    data = json.load(f)

# Extract metrics
insert_sizes = [50, 75, 100, 150, 200]
build_times = [r['build_time'] for r in data]
recalls = [r['recall@10'] for r in data]

# Plot
fig, ax1 = plt.subplots()
ax1.plot(insert_sizes, build_times, 'b-', label='Build Time')
ax1.set_xlabel('insert_list_size')
ax1.set_ylabel('Build Time (s)', color='b')

ax2 = ax1.twinx()
ax2.plot(insert_sizes, recalls, 'r-', label='Recall@10')
ax2.set_ylabel('Recall@10', color='r')

plt.title('Build Time vs Recall Trade-off')
plt.show()
```

### 2. Find Pareto Frontier

```python
# Find points where no other point is better in all dimensions
pareto_points = []
for point in results:
    is_pareto = True
    for other in results:
        if (other.recall >= point.recall and
            other.qps >= point.qps and
            other.build_time <= point.build_time and
            (other.recall > point.recall or
             other.qps > point.qps or
             other.build_time < point.build_time)):
            is_pareto = False
            break
    if is_pareto:
        pareto_points.append(point)
```

### 3. Extrapolate to 500k

```python
import numpy as np

# Fit power law: build_time = a * n^b
sizes = [10000, 25000, 50000, 100000, 200000]
times = [40, 120, 280, 640, 1500]

# Log-log regression
log_sizes = np.log(sizes)
log_times = np.log(times)
b, log_a = np.polyfit(log_sizes, log_times, 1)
a = np.exp(log_a)

# Predict for 500k
predicted_500k = a * (500000 ** b)
print(f"Predicted build time for 500k: {predicted_500k:.0f}s ({predicted_500k/60:.1f}min)")
```

## Decision Matrix

Use this to choose defaults based on use case:

| Use Case                 | Dataset Size | max_neighbors | insert_list | search_list | Why                            |
| ------------------------ | ------------ | ------------- | ----------- | ----------- | ------------------------------ |
| Development/Testing      | <50k         | 24            | 50          | 100         | Fast builds, size matters less |
| Production (balanced)    | 50k-500k     | 32            | 100         | 150         | Sweet spot for most users      |
| Production (high recall) | 100k+        | 48            | 150         | 200         | Critical search quality        |
| Large scale (500k+)      | 500k+        | 64            | 200         | 300         | Need high connectivity         |
| Low-latency queries      | Any          | 24            | 100         | 75          | Minimize query time            |

## Next Steps

1. **Run scaling-test.json** to find crossover point vs brute-force
2. **Run param sweeps** to find optimal values
3. **Update DEFAULT\_\* constants** in `src/diskann_api.c`
4. **Update PARAMETERS.md** with measured results
5. **Add tuning results** to README.md

## Expected Timeline

- Insert list sweep (50k × 5 configs): ~30 minutes
- Max neighbors sweep (50k × 4 configs): ~25 minutes
- Scaling test (5 sizes × 2 runners): ~90 minutes
- **Total:** ~2-3 hours for complete tuning suite

## Validation

After choosing new defaults, validate with:

```bash
# Test on realistic data
npm run bench:standard  # Should hit target recall/QPS

# Test at scale
npm run bench -- --profile=profiles/scaling-test.json

# Verify all tests still pass
make clean && make test
npm run test:ts
```
