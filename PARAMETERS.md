# DiskANN Index Parameters

## Overview

DiskANN indices store configuration parameters in the `<tablename>_metadata` shadow table. Understanding which parameters can be changed after index creation is critical for tuning performance.

## Parameter Mutability

### 🔒 **IMMUTABLE** (require full index rebuild)

These parameters determine the physical storage layout and cannot be changed without recreating the index.

#### `dimensions` (uint32)

- **What:** Vector dimensionality (e.g., 64, 128, 256, 768, 1536)
- **Default:** None (required)
- **Why immutable:** Determines binary layout of nodes and edges
- **Recommended values:**
  - Small models (MiniLM, etc.): 384
  - Medium models (BERT, etc.): 768
  - Large models (OpenAI): 1536
  - Custom embeddings: Match your model's output

#### `metric` (uint8)

- **What:** Distance function for similarity search
- **Default:** `euclidean` (0)
- **Options:**
  - `euclidean` (0): L2 distance, best for general use
  - `cosine` (1): Normalized dot product, good for text embeddings
  - `dot` (2): Inner product, for pre-normalized vectors
- **Why immutable:** Changing distance function would invalidate all edge relationships
- **Recommended:** Use `cosine` for text embeddings, `euclidean` for general vectors

#### `max_neighbors` (uint32)

- **What:** Maximum edges per node in the graph
- **Default:** 64
- **Why immutable:** Determines block size via formula:
  ```
  node_overhead = 16 + (dimensions × 4)
  edge_overhead = (dimensions × 4) + 16
  margin = max_neighbors + (max_neighbors / 10)
  block_size = node_overhead + (margin × edge_overhead)
  ```
- **Recommended values:**
  - Small datasets (<10k): 32
  - Medium datasets (10k-100k): 64
  - Large datasets (100k-1M): 64-96
  - Very large (1M+): 96-128
- **Trade-offs:**
  - Higher = better recall, slower build, larger index
  - Lower = faster build, smaller index, lower recall

#### `block_size` (uint32)

- **What:** SQLite BLOB size in bytes for storing nodes
- **Default:** 0 (auto-calculate from dimensions × max_neighbors)
- **Why immutable:** SQLite BLOB handles are opened with fixed size
- **Recommended:** Always use auto-calculate (0) unless you have specific requirements
- **Manual override:** Only if you need precise control over index size

### ⚠️ **SEMI-MUTABLE** (changeable but with caveats)

These parameters affect graph construction quality. Changing mid-build creates inconsistency.

#### `insert_list_size` (uint32)

- **What:** Beam width during INSERT (how many candidates to explore when adding a node)
- **Default:** 200
- **Stored in:** Metadata table
- **How to change:**
  ```sql
  UPDATE tablename_metadata SET value=100 WHERE key='insert_list_size';
  -- Then clear and rebuild graph:
  DELETE FROM tablename_shadow;
  -- Re-insert all vectors
  ```
- **When to change:**
  - **Before building:** Set via CREATE VIRTUAL TABLE parameter
  - **After building:** Requires clearing and reinserting all vectors
- **Recommended values:**
  - Small datasets (<10k): 50-100
  - Medium datasets (10k-100k): 100-150
  - Large datasets (100k+): 150-200
- **Trade-offs:**
  - Higher = better graph quality, slower builds, more I/O
  - Lower = faster builds, lower recall
- **Performance impact:** Each insert does `insert_list_size` BLOB reads = major build time factor

#### `pruning_alpha` (double, stored as int×1000)

- **What:** Diversity parameter for edge pruning (higher = more diverse neighbors)
- **Default:** 1.4
- **Stored in:** Metadata table as `pruning_alpha_x1000` (e.g., 1400 = 1.4)
- **How to change:**
  ```sql
  UPDATE tablename_metadata SET value=1200 WHERE key='pruning_alpha_x1000';
  -- Clear and rebuild graph
  ```
- **Recommended values:**
  - Standard: 1.2-1.4
  - High diversity needed: 1.5-2.0
  - Maximum connectivity: 1.0-1.2
- **Trade-offs:**
  - Higher = more diverse edges, better recall on diverse queries
  - Lower = more similar edges, may improve recall on clustered data
- **Note:** Changing mid-build creates edges with different pruning criteria (inconsistent graph quality)

### ✅ **RUNTIME MUTABLE** (can change per-query)

These parameters control search behavior and can be overridden without rebuilding.

#### `search_list_size` (uint32)

- **What:** Beam width during search (how many candidates to explore per query)
- **Default:** 100
- **Stored in:** Metadata table (but can be overridden)
- **How to override:**

  ```sql
  -- Use default (100)
  SELECT rowid, distance FROM vectors WHERE vector MATCH ? AND k = 10;

  -- Override to 300 for higher recall
  SELECT rowid, distance FROM vectors
  WHERE vector MATCH ? AND k = 10 AND search_list_size = 300;
  ```

- **Recommended values:**
  - Fast queries: 50-100
  - Balanced: 100-200
  - High recall: 200-500
  - Maximum recall: 500-1000
- **Trade-offs:**
  - Higher = better recall, slower queries
  - Lower = faster queries, lower recall
- **Rule of thumb:** `search_list_size ≥ k × 2` for good recall
- **Performance impact:** Linear with beam width (2x beam = ~2x query time)

## Parameter Selection Guide

### Use Case: Text Embeddings (384D-1536D)

```sql
CREATE VIRTUAL TABLE embeddings USING diskann(
  dimension=768,              -- Match your model
  metric=cosine,              -- Standard for text
  max_neighbors=64,           -- Good balance
  insert_list_size=100,       -- Faster builds
  search_list_size=150        -- Good recall
);
```

**Expected performance:**

- Build: ~100ms per 1k vectors
- Recall@10: 90-95%
- QPS: 500-1000 (depends on hardware)

### Use Case: Image Embeddings (512D-2048D)

```sql
CREATE VIRTUAL TABLE images USING diskann(
  dimension=512,
  metric=euclidean,           -- Better for image features
  max_neighbors=96,           -- Higher connectivity
  insert_list_size=150,
  search_list_size=200
);
```

**Expected performance:**

- Build: ~150ms per 1k vectors
- Recall@10: 92-97%
- QPS: 300-600

### Use Case: Small Dataset (<10k vectors)

```sql
CREATE VIRTUAL TABLE small USING diskann(
  dimension=128,
  metric=euclidean,
  max_neighbors=32,           -- Less overhead
  insert_list_size=50,        -- Faster builds
  search_list_size=100
);
```

**Note:** For datasets <10k, sqlite-vec (brute force) may be faster. DiskANN's overhead only pays off at scale.

### Use Case: Large Dataset (100k-1M vectors)

```sql
CREATE VIRTUAL TABLE large USING diskann(
  dimension=256,
  metric=cosine,
  max_neighbors=96,           -- Better connectivity at scale
  insert_list_size=200,       -- Higher quality graph
  search_list_size=300        -- Maintain recall
);
```

**Expected performance:**

- Build: ~10-20 seconds per 10k vectors
- Recall@10: 93-98%
- QPS: 100-300

## Changing Parameters on Existing Indices

### Safe Changes (No Rebuild Required)

**Override search_list_size per query:**

```sql
-- TypeScript
const results = searchNearest(db, 'vectors', query, 10, {
  searchListSize: 300  // Override default
});

-- SQL
SELECT rowid, distance FROM vectors
WHERE vector MATCH ? AND k = 10 AND search_list_size = 300;
```

### Unsafe Changes (Require Full Rebuild)

**To change dimensions, metric, max_neighbors, or block_size:**

```sql
-- 1. Export vectors
CREATE TEMP TABLE backup AS
SELECT rowid, vector FROM old_vectors_shadow;

-- 2. Drop old index
DROP TABLE old_vectors;

-- 3. Create new index with new parameters
CREATE VIRTUAL TABLE new_vectors USING diskann(
  dimension=256,  -- Changed from 128
  -- ... other params
);

-- 4. Re-insert vectors
INSERT INTO new_vectors (rowid, vector)
SELECT rowid, vector FROM backup;

-- 5. Verify
SELECT COUNT(*) FROM new_vectors_shadow;
```

### Partial Rebuild (Change Graph Parameters)

**To change insert_list_size or pruning_alpha:**

```sql
-- 1. Update metadata
UPDATE vectors_metadata SET value=100 WHERE key='insert_list_size';
UPDATE vectors_metadata SET value=1200 WHERE key='pruning_alpha_x1000';

-- 2. Clear graph but keep vectors
DELETE FROM vectors_shadow;

-- 3. Re-insert vectors (faster than full rebuild since vectors exist)
INSERT INTO vectors (rowid, vector)
SELECT rowid, vector FROM vectors_shadow_backup;
```

**Note:** This still requires re-building the graph structure, but avoids re-computing embeddings.

## Monitoring Parameter Performance

### Check Current Parameters

```sql
-- View all parameters
SELECT key, value FROM vectors_metadata ORDER BY key;

-- Check specific parameter
SELECT value FROM vectors_metadata WHERE key='search_list_size';
```

### Benchmark Queries

```sql
-- Test different search_list_size values
.timer on

-- Baseline
SELECT rowid, distance FROM vectors
WHERE vector MATCH ? AND k=10;

-- Higher recall
SELECT rowid, distance FROM vectors
WHERE vector MATCH ? AND k=10 AND search_list_size=300;

-- Maximum recall
SELECT rowid, distance FROM vectors
WHERE vector MATCH ? AND k=10 AND search_list_size=1000;
```

### Measure Recall

See `benchmarks/` directory for comprehensive recall measurement tools.

```bash
cd benchmarks
npm run bench:quick -- --profile=recall-sweep
```

## Common Mistakes

### ❌ Changing max_neighbors After Building

```sql
-- This will NOT work - max_neighbors determines block_size!
UPDATE vectors_metadata SET value=128 WHERE key='max_neighbors';
```

**Why:** Block size is fixed at index creation. Changing max_neighbors creates mismatch.

### ❌ Using insert_list_size < k

```sql
CREATE VIRTUAL TABLE bad USING diskann(
  dimension=128,
  insert_list_size=5  -- Too small!
);
-- Insert with k=10 will only explore 5 candidates
```

**Rule:** `insert_list_size ≥ max_neighbors` recommended

### ❌ Extreme search_list_size Values

```sql
-- Too low - poor recall
WHERE vector MATCH ? AND k=100 AND search_list_size=10

-- Too high - no benefit past ~10x k
WHERE vector MATCH ? AND k=10 AND search_list_size=10000
```

**Sweet spot:** `search_list_size = k × 2` to `k × 10`

## Performance Tuning Checklist

- [ ] `dimensions` matches your embedding model exactly
- [ ] `metric` appropriate for your data (cosine for text, euclidean for images)
- [ ] `max_neighbors` balanced for dataset size (32-128 range)
- [ ] `insert_list_size` appropriate for build time tolerance (50-200)
- [ ] `search_list_size` tuned for recall/speed tradeoff (test with different values)
- [ ] Block size auto-calculated (unless you have specific needs)
- [ ] Tested recall on representative queries before production
- [ ] Monitored query latency under load

## Further Reading

- **Algorithm Details:** See DiskANN paper (NeurIPS 2019)
- **Benchmarks:** `benchmarks/README.md`
- **Index Format:** `src/diskann_node.h` (binary layout documentation)
- **Build Optimization:** `_todo/20260211-build-speed-optimization.md` (if available)
