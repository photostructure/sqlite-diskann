# DiskANN Index Size Reduction (Optional)

## Summary

Reduce DiskANN index size from 38x raw data to 15-22x through scalar quantization and dynamic block sizing. Current 989MB for 25k 256D vectors is the cost of pre-allocated graph edges (36KB per node). Two compression techniques can reduce to ~500-600MB with <2% recall loss. **ONLY pursue after build speed optimization proves insufficient.**

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

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- `src/diskann_node.h` - Node binary format (V2 current, V3 planned)
- `src/diskann_node.c` - Node serialization and vector storage
- `src/diskann_api.c` - Block size calculation (lines 48-84)
- `src/diskann_insert.c` - Vector insertion path
- `src/diskann_search.c` - Distance calculation and search
- Research: Scalar quantization papers (see Notes section)

## Description

**Problem:** Block size fix created large indices:

- 989MB for 25k vectors @ 256D (38x vs 26MB raw)
- Projected: ~4GB for 100k vectors
- Root cause: Pre-allocated 35 edge slots × 1,040 bytes = 36KB per node
- 76% utilization (good, not bloat), but still large

**Why this is OPTIONAL:**

- Build speed is the critical blocker (addressed by separate TPP)
- 38x size is the actual cost of graph structure
- Production may tolerate larger indices if queries are fast
- Size reduction requires compression (risk to recall)

**Constraints:**

- Must maintain ≥92% recall (allow 2-3% degradation maximum)
- Breaking change: format_version=3
- Must provide migration path from V2→V3
- Quantization/dequantization must not destroy query performance

**Success Criteria:**

- Index size: <600MB for 25k vectors (15-22x ratio)
- Recall: ≥92% @ k=10 (max 3% loss from 95% baseline)
- QPS: >80% of V2 baseline (allow 20% slowdown)
- Migration tool working and tested

## Tribal Knowledge

**Space breakdown (25k @ 256D with maxDegree=32):**

- Block size: 40KB per node (auto-calculated from dimensions × max_neighbors)
- Node overhead: 16 bytes metadata + 1,024 bytes vector = 1,040 bytes
- Edge overhead: 1,024 bytes vector + 16 bytes metadata = 1,040 bytes
- Max edges: 35 (32 + 10% margin)
- Total: 1,040 + (35 × 1,040) = 37,440 bytes per node
- Aligned to 4KB: 40KB blocks
- 25k nodes × 40KB = 976MB (observed: 989MB, close match)

**Waste analysis:**

- Unused edge slots: 24% (35 allocated, ~27 used avg)
- Float32 precision: 58% of space (could quantize to int16/int12)
- Fixed-size blocks: 10% (leaves could use smaller blocks)
- Rowid encoding: <1% (could use varint, negligible)

**Why 38x seems large:**

- sqlite-vec uses brute force (no graph): 26MB (just vectors)
- DiskANN stores graph edges WITH vectors: 40x data per node
- Each edge stores full 256D vector (1,024 bytes) for distance calc
- This is fundamental to graph-based ANN, not a bug

**Quantization research (Explore agent findings):**

- SQ int16: 50% vector space reduction, 0.5-1% recall loss
- SQ int12: 62.5% reduction, 1.5-2% recall loss
- Store min/max per vector (8 bytes) for rescaling
- Asymmetric distance: query stays float32, index uses int16
- Expected: 989MB → 598MB with int16 (38.8% total reduction)

## Solutions

### Option 1: Scalar Quantization (int16) - RECOMMENDED

**Approach:** Compress float32→int16 (65,536 levels), store min/max for rescaling

**Pros:**

- 38.8% size reduction (989MB → 598MB)
- Well-proven technique (GaussDB, Milvus use this)
- 0.5-1% recall loss (acceptable: 95% → 94%)
- Dequantization fast (vectorizable with SIMD)

**Cons:**

- 2-3 weeks implementation (format V3)
- Breaking change, requires migration tool
- Slight QPS loss from dequantization overhead (~10-20%)

**Status:** RECOMMENDED first step if pursuing size reduction

**Implementation:**

- New node format: int16 arrays for vectors
- Quantization: `(value - min) / (max - min) × 65535`
- Store min/max: 8 bytes overhead per node
- Dequantize on read: `min + (quantized / 65535.0) × (max - min)`

---

### Option 2: Dynamic Block Sizing - COMPLEMENTARY

**Approach:** Use smaller blocks (20KB) for leaf/low-degree nodes

**Pros:**

- 10% additional size reduction (598MB → 538MB combined with SQ)
- Lossless (no recall impact)
- Relatively simple (3-5 days)

**Cons:**

- Requires per-row block_size metadata
- Complicates BLOB I/O layer
- Less impactful than quantization alone

**Status:** COMPLEMENTARY - Do after SQ if still not small enough

---

### Option 3: More Aggressive Quantization (int12) - IF NEEDED

**Approach:** 12-bit quantization (4,096 levels)

**Pros:**

- Additional 12% reduction (538MB → 470MB)
- Marginal implementation over int16

**Cons:**

- Additional 0.5-1% recall loss (cumulative 1.5-2%)
- Bit-packing complexity (not byte-aligned)

**Status:** IF NEEDED to reach 15x target (380MB)

---

### Option 4: Reduce max_neighbors 32→28 - PARAMETER TUNE

**Approach:** Lower maxDegree config value

**Pros:**

- 12.5% reduction with parameter change only
- Quick to test

**Cons:**

- May hurt connectivity at 100k+ scale
- Reduced graph flexibility

**Status:** LOW PRIORITY - Test if other approaches insufficient

---

### Option 5: Product Quantization (PQ) - TOO COMPLEX

**Approach:** Vector quantization with codebook (like Faiss)

**Pros:**

- 75%+ size reduction possible
- Used in production systems

**Cons:**

- Very complex (4-6 weeks implementation)
- Requires training phase (codebook generation)
- Significant recall loss without careful tuning

**Status:** REJECTED - Not worth complexity for 25k-100k scale

## Tasks

### Phase 1: SQ int16 Design (Week 1)

#### Research & Design (2-3 days)

- [ ] Study quantization in `src/diskann_node.c` (current float32 storage)
- [ ] Design V3 node format:
  - int16_t arrays for node + edge vectors
  - float min, max fields (8 bytes overhead)
  - Keep metadata format unchanged
- [ ] Calculate new block sizes:
  - Node: 16 + 512 (int16×256D) + 8 (min/max) = 536 bytes
  - Edges: 35 × (512 + 16) = 18,480 bytes
  - Total: ~19KB (down from 40KB)
- [ ] Document format in `src/diskann_node.h` comments

#### API Design (1 day)

- [ ] Design quantization functions:
  - `int quantize_vector(const float *in, int16_t *out, int dim, float *min, float *max)`
  - `void dequantize_vector(const int16_t *in, float *out, int dim, float min, float max)`
- [ ] Design distance function updates:
  - Keep asymmetric: query=float32, index=int16
  - Dequantize during distance calculation
- [ ] Update `calculate_block_size()` for V3 format

#### Format Versioning (1 day)

- [ ] Add CURRENT_FORMAT_VERSION=3 to `src/diskann_api.c`
- [ ] Update metadata storage to record format_version
- [ ] Design migration: V2→V3 requires rebuild (no in-place)

### Phase 2: Implementation (Week 2)

#### Core Quantization (2-3 days)

- [ ] Implement `quantize_vector()` in `src/diskann_node.c`:
  - Find min/max in single pass
  - Scale to int16 range
  - Handle edge case: constant vector (min==max)
- [ ] Implement `dequantize_vector()`:
  - Rescale from int16→float32
  - Inline for performance
- [ ] Write unit tests: `tests/c/test_quantization.c`
  - Test round-trip: float→int16→float
  - Measure quantization error
  - Test edge cases (zeros, negatives, large values)

#### Node Format Update (2-3 days)

- [ ] Update `node_bin_init()` to quantize on write
- [ ] Update `node_bin_vector()` to dequantize on read
- [ ] Update `node_bin_edge()` to handle int16 storage
- [ ] Add min/max storage in node metadata section

#### Insert Path (1-2 days)

- [ ] Update `diskann_insert.c`:
  - Quantize new vector before `node_bin_init()`
  - Store min/max in node
- [ ] Ensure pruning still works with quantized distances

#### Search Path (1-2 days)

- [ ] Update `diskann_search.c`:
  - Dequantize during beam expansion
  - Cache dequantized vectors if performance critical
- [ ] Update distance functions to handle int16

### Phase 3: Testing & Validation (Week 3)

#### Unit Testing (2 days)

- [ ] Test quantization accuracy:
  - Max error should be <0.01 for normalized vectors
- [ ] Test node read/write round-trip
- [ ] Test edge insertion with quantized vectors
- [ ] Run: `make test` - all tests pass

#### Integration Testing (2 days)

- [ ] Create V3 index, insert 10k vectors
- [ ] Search and compare recall vs V2
- [ ] Test reopen (persistence)
- [ ] Run: `make asan` - clean
- [ ] Run: `make valgrind` - no leaks

#### Benchmark Comparison (2-3 days)

- [ ] Add `profiles/quantization-comparison.json`:
  - Variants: V2 float32, V3 int16
  - Measure: size, recall, QPS
- [ ] Run: `npm run bench profiles/quantization-comparison.json`
- [ ] Expected results:
  - Size: 989MB → 598MB (39% reduction)
  - Recall: 95% → 94% (<1% loss)
  - QPS: 500 → 400+ (>80% of baseline)
- [ ] Document trade-offs in commit message

#### Migration Tool (2 days)

- [ ] Write TypeScript migration utility: `scripts/migrate-v2-to-v3.ts`
  - Export all vectors from V2 index
  - Create V3 index with quantization
  - Bulk insert
  - Verify recall
- [ ] Test migration on 10k dataset
- [ ] Document in README

### Optional: Phase 4: Dynamic Block Sizing (3-5 days)

**ONLY if SQ int16 insufficient and user approves**

- [ ] Add `block_size` column to shadow table
- [ ] Implement `choose_block_size(n_edges)` heuristic
- [ ] Update BLOB I/O to read size from metadata
- [ ] Test with mixed block sizes (16KB, 20KB, 24KB)
- [ ] Run benchmarks: expect 10% additional reduction

**Verification:**

```bash
# After Phase 3
cd benchmarks
rm -rf datasets/synthetic/*.db
npm run prepare
npm run bench profiles/quantization-comparison.json

# Target metrics:
# - V3 size: <650MB (33% reduction from 989MB)
# - V3 recall: ≥94% (<1% loss from 95%)
# - V3 QPS: ≥400 (>80% of 500 baseline)

# Stress testing:
make clean && make test
make clean && make asan
make clean && make valgrind
```

## Notes

### Session 2026-02-11: Size Analysis

**Current state:**

- 25k @ 256D, maxDegree=32: 989MB (38x raw 26MB)
- Block size: 40KB (auto-calculated, necessary for connectivity)
- Space utilization: 76% (good, not wasteful)
- But still large compared to brute-force sqlite-vec

**User feedback:**

- "38x larger index and glacial build times not acceptable"
- "Need BOTH reasonable recall AND not abysmal performance"
- Build speed is priority #1 (separate TPP)
- Index size is priority #2 (this TPP)

**Research findings:**

- Scalar quantization (int16) proven in production systems
- GaussDB-Vector paper: 50% size reduction, <1% recall loss
- Milvus uses SQ as default compression
- Asymmetric distance (query float32, index int16) maintains quality

**Decision:**

- Mark this TPP as OPTIONAL
- User should complete build-speed-optimization TPP first
- Revisit this only if:
  1. Build speed is acceptable (target met)
  2. Index size still blocks deployment
  3. User has 2-3 weeks available

### Research: Scalar Quantization Papers

**Key papers:**

1. "Product Quantization for Nearest Neighbor Search" (Jegou 2011)
2. "The Inverted Multi-Index" (Babenko 2012)
3. "Revisiting Additive Quantization" (Martinez 2016)
4. GaussDB-Vector VLDB 2025 - practical production system

**Consensus approach:**

- Scalar quantization simplest, most robust
- 16-bit sufficient for 256D vectors
- Store per-vector min/max (not global) for accuracy
- Asymmetric distance preserves quality

**Expected impact:**

- Size: 40-50% reduction
- Recall: 0.5-1% loss
- QPS: 10-20% slower (dequantization overhead)

### Future: Advanced Techniques (Out of Scope)

**If size still critical after SQ+dynamic:**

- Product Quantization (PQ): 75% reduction, complex
- Compression via shared codebook: 80% reduction, training needed
- Separate edge table: flexible but breaks BLOB model

**Recommendation:**

- Accept 20-22x size ratio after SQ
- Focus optimization effort on build speed and query performance
- Size is secondary concern if recall and speed are good
