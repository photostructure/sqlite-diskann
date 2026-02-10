# Test Strategy: sqlite-diskann

**Goal:** Ensure DiskANN extraction works correctly as both a C SQLite extension and an npm package.

## Two-Layer Testing Approach

### Layer 1: C Tests (Extension Core)
Test the SQLite extension implementation in C.

### Layer 2: TypeScript/Node.js Tests (npm Package)
Test the npm package wrapper and Node.js integration.

---

## Layer 1: C Tests

### Test Framework
**Unity** - Lightweight C testing framework (as per TDD.md)

### Test Categories

#### 1.1 Unit Tests: Vector Operations
**File:** `tests/c/test_vector_ops.c`

Test vector distance calculations and normalization:
- Euclidean distance (2D, 3D, high-dimensional)
- Cosine similarity
- Dot product
- Vector normalization
- Edge cases: zero vectors, unit vectors, high dimensions (768d)

```c
void test_euclidean_distance_2d(void);
void test_euclidean_distance_768d(void);
void test_normalize_vector_success(void);
void test_normalize_zero_vector_fails(void);
```

#### 1.2 Unit Tests: DiskANN Algorithm
**File:** `tests/c/test_diskann_core.c`

Test core DiskANN graph operations:
- Graph node creation (4KB blocks)
- Node neighbor management
- Graph traversal
- Search algorithm
- Index building

```c
void test_create_graph_node(void);
void test_add_neighbor_to_node(void);
void test_graph_search_finds_nearest(void);
void test_index_build_correctness(void);
```

#### 1.3 Unit Tests: Endianness Helpers
**File:** `tests/c/test_endianness.c`

Test little-endian read/write helpers:
- `readLE32()` / `writeLE32()`
- `readLE64()` / `writeLE64()`
- Cross-platform correctness

```c
void test_readLE32_correctness(void);
void test_writeLE32_roundtrip(void);
void test_readLE64_correctness(void);
void test_writeLE64_roundtrip(void);
```

#### 1.4 Integration Tests: SQLite Extension
**File:** `tests/c/test_sqlite_extension.c`

Test SQLite extension loading and virtual table:

**Extension Loading:**
```c
void test_extension_loads_successfully(void);
void test_extension_init_returns_ok(void);
```

**Virtual Table Creation:**
```c
void test_create_virtual_table(void);
void test_create_table_with_dimension_param(void);
void test_create_table_with_metric_param(void);
void test_create_table_invalid_params_fails(void);
```

**Data Operations:**
```c
void test_insert_single_vector(void);
void test_insert_multiple_vectors(void);
void test_insert_invalid_dimension_fails(void);
void test_query_knn_returns_results(void);
void test_query_with_k_parameter(void);
```

**Shadow Table Persistence:**
```c
void test_shadow_table_created(void);
void test_graph_nodes_persisted_as_blobs(void);
void test_4kb_node_alignment(void);
```

#### 1.5 Memory Safety Tests
**Files:** Run existing tests under sanitizers

**AddressSanitizer (ASan):**
```bash
make test-asan
# Detects: use-after-free, buffer overflows, memory leaks
```

**Valgrind:**
```bash
make test-valgrind
# Detects: memory leaks, invalid reads/writes
```

**UndefinedBehaviorSanitizer (UBSan):**
```bash
make test-ubsan
# Detects: integer overflow, null pointer dereference
```

#### 1.6 Scale Tests
**File:** `tests/c/test_scale.c`

Test with realistic dataset sizes:
- 10K vectors (quick smoke test)
- 100K vectors (medium scale)
- 1M vectors (production scale - if CI allows)
- 5M vectors (manual test, not CI)

```c
void test_index_10k_vectors(void);
void test_query_100k_vectors_latency(void);
void test_recall_accuracy_1m_vectors(void);
```

#### 1.7 Correctness Tests
**File:** `tests/c/test_correctness.c`

Verify ANN results match brute-force:
- Small dataset (1K vectors) - compare ANN vs brute-force
- Measure recall@k (should be >95%)
- Verify result ordering is correct

```c
void test_recall_vs_bruteforce_1k(void);
void test_recall_rate_above_95_percent(void);
```

---

## Layer 2: TypeScript/Node.js Tests

### Test Framework
**Vitest** or **Jest** - Modern TypeScript testing

### Test Categories

#### 2.1 Package Loading Tests
**File:** `tests/ts/extension-loading.test.ts`

Test that the native extension loads correctly:

```typescript
import { describe, it, expect } from 'vitest';
import Database from 'better-sqlite3';
import path from 'node:path';

describe('Extension Loading', () => {
  it('loads extension on Linux', () => {
    const db = new Database(':memory:');
    const extPath = path.join(__dirname, '../../dist/diskann.so');
    db.loadExtension(extPath);
    // Should not throw
  });

  it('loads extension on macOS', () => {
    // Same but .dylib
  });

  it('loads extension on Windows', () => {
    // Same but .dll
  });

  it('fails gracefully with helpful error on missing binary', () => {
    const db = new Database(':memory:');
    expect(() => {
      db.loadExtension('/nonexistent/path.so');
    }).toThrow(/extension/i);
  });
});
```

#### 2.2 API Wrapper Tests
**File:** `tests/ts/api-wrapper.test.ts`

Test TypeScript wrapper API:

```typescript
import { DiskANNIndex } from 'sqlite-diskann';

describe('DiskANN API', () => {
  it('creates index with dimension parameter', () => {
    const db = new Database(':memory:');
    const index = new DiskANNIndex(db, 'embeddings', { dimension: 768 });
    expect(index).toBeDefined();
  });

  it('inserts vectors', async () => {
    const index = new DiskANNIndex(db, 'embeddings', { dimension: 128 });
    await index.insert(1, new Float32Array(128));
    // Should not throw
  });

  it('queries k-nearest neighbors', async () => {
    const index = new DiskANNIndex(db, 'embeddings', { dimension: 128 });
    await index.insert(1, new Float32Array(128));
    await index.insert(2, new Float32Array(128));

    const results = await index.query(new Float32Array(128), { k: 5 });
    expect(results).toHaveLength(2); // Only 2 vectors inserted
    expect(results[0]).toHaveProperty('id');
    expect(results[0]).toHaveProperty('distance');
  });
});
```

#### 2.3 TypeScript Types Tests
**File:** `tests/ts/types.test.ts`

Test TypeScript type definitions:

```typescript
import { expectTypeOf } from 'vitest';
import type { DiskANNIndex, QueryResult, IndexOptions } from 'sqlite-diskann';

describe('TypeScript Types', () => {
  it('has correct IndexOptions type', () => {
    expectTypeOf<IndexOptions>().toMatchTypeOf<{
      dimension: number;
      metric?: 'euclidean' | 'cosine' | 'dot';
    }>();
  });

  it('has correct QueryResult type', () => {
    expectTypeOf<QueryResult>().toMatchTypeOf<{
      id: number;
      distance: number;
    }>();
  });

  it('query returns array of results', () => {
    const index = {} as DiskANNIndex;
    expectTypeOf(index.query).returns.toEqualTypeOf<Promise<QueryResult[]>>();
  });
});
```

#### 2.4 Cross-Platform Binary Tests
**File:** `tests/ts/platform-binaries.test.ts`

Test that correct binary is loaded per platform:

```typescript
describe('Platform Binary Selection', () => {
  it('loads linux-x64 binary on Linux x64', () => {
    // Mock process.platform and process.arch
    const expectedPath = 'dist/linux-x64/diskann.so';
    // Verify correct binary is selected
  });

  it('loads darwin-arm64 binary on macOS ARM64', () => {
    const expectedPath = 'dist/darwin-arm64/diskann.dylib';
  });

  it('loads win32-x64 binary on Windows x64', () => {
    const expectedPath = 'dist/win32-x64/diskann.dll';
  });

  it('throws helpful error on unsupported platform', () => {
    // Mock unsupported platform
    expect(() => loadExtension()).toThrow(/not supported/i);
  });
});
```

#### 2.5 Integration Tests (End-to-End)
**File:** `tests/ts/integration.test.ts`

Test real-world usage scenarios:

```typescript
describe('Integration Tests', () => {
  it('indexes and queries 10K vectors', async () => {
    const db = new Database(':memory:');
    const index = new DiskANNIndex(db, 'test', { dimension: 128 });

    // Insert 10K random vectors
    for (let i = 0; i < 10000; i++) {
      const vec = new Float32Array(128);
      vec.fill(Math.random());
      await index.insert(i, vec);
    }

    // Query
    const query = new Float32Array(128).fill(Math.random());
    const results = await index.query(query, { k: 10 });

    expect(results).toHaveLength(10);
    // Verify distances are sorted ascending
    for (let i = 1; i < results.length; i++) {
      expect(results[i].distance).toBeGreaterThanOrEqual(results[i-1].distance);
    }
  });

  it('persists index to disk and reopens', async () => {
    const dbPath = '/tmp/test-diskann.db';

    // Create and populate index
    {
      const db = new Database(dbPath);
      const index = new DiskANNIndex(db, 'test', { dimension: 128 });
      await index.insert(1, new Float32Array(128).fill(1.0));
      db.close();
    }

    // Reopen and query
    {
      const db = new Database(dbPath);
      const index = new DiskANNIndex(db, 'test', { dimension: 128 });
      const results = await index.query(new Float32Array(128).fill(1.0), { k: 1 });
      expect(results[0].id).toBe(1);
      db.close();
    }
  });
});
```

#### 2.6 npm Package Tests
**File:** `tests/ts/package.test.ts`

Test npm package structure:

```typescript
describe('npm Package', () => {
  it('has correct package.json exports', async () => {
    const pkg = await import('../../package.json');
    expect(pkg.exports).toBeDefined();
    expect(pkg.exports['.']).toBeDefined();
  });

  it('includes prebuilt binaries in package', () => {
    // Verify dist/ folder structure
    const platforms = ['linux-x64', 'darwin-x64', 'darwin-arm64', 'win32-x64'];
    for (const platform of platforms) {
      const binaryPath = path.join(__dirname, '../../dist', platform);
      expect(fs.existsSync(binaryPath)).toBe(true);
    }
  });

  it('has TypeScript type definitions', () => {
    const dtsPath = path.join(__dirname, '../../dist/index.d.ts');
    expect(fs.existsSync(dtsPath)).toBe(true);
  });
});
```

---

## Test Data Fixtures

### C Test Fixtures
**Location:** `tests/c/fixtures/`

- `vectors_1k.bin` - 1K random vectors (128d) for correctness tests
- `vectors_10k.bin` - 10K random vectors (768d) for scale tests
- `queries_100.bin` - 100 test queries

**Generation script:** `tests/c/fixtures/generate.py`

### TypeScript Test Fixtures
**Location:** `tests/ts/fixtures/`

- `sample-vectors.json` - Small dataset for API tests
- `clip-embeddings.json` - Sample CLIP vectors (512d)

---

## Test Execution

### C Tests

```bash
# All C tests
make test-c

# Specific test file
make test-c-unit

# With AddressSanitizer
make test-asan

# With Valgrind
make test-valgrind

# Coverage report
make coverage
```

### TypeScript Tests

```bash
# All TypeScript tests
npm test

# Watch mode
npm test -- --watch

# Specific test file
npm test extension-loading

# Coverage
npm run test:coverage
```

### CI Pipeline Tests

```bash
# Run in GitHub Actions
npm run test:ci

# Includes:
# - C unit tests
# - C integration tests
# - TypeScript unit tests
# - TypeScript integration tests
# - Cross-platform binary validation
# - Memory leak checks (Linux only)
```

---

## Test Coverage Goals

### C Code Coverage
- **Target:** >80% line coverage
- **Critical paths:** 100% coverage
  - Vector operations
  - Graph search algorithm
  - SQLite extension interface
  - Memory management

### TypeScript Code Coverage
- **Target:** >90% line coverage (easier in TypeScript)
- **Critical paths:** 100% coverage
  - Extension loading
  - API wrapper
  - Error handling

---

## Performance Benchmarks

**File:** `tests/bench/benchmark.ts`

Not tests per se, but measure performance:

```typescript
describe('Performance Benchmarks', () => {
  it('queries 100K vectors in <10ms (avg)', async () => {
    // Measure query latency
  });

  it('indexes 1M vectors in <5 minutes', async () => {
    // Measure indexing time
  });

  it('recall@10 is >95% on 1M vectors', async () => {
    // Measure recall rate
  });
});
```

Run separately: `npm run bench`

---

## Test Dependencies

### C Tests
- Unity testing framework
- SQLite3 library
- Valgrind (for memory tests)
- GCC/Clang with sanitizers

### TypeScript Tests
- Vitest or Jest
- better-sqlite3 (SQLite driver)
- TypeScript
- Node.js 18+

---

## Reference Projects

Follow test patterns from:
- `../fs-metadata` - Cross-platform C extension tests
- `../node-sqlite` - npm package structure tests
- ❌ NOT `../sqlite-vec` - avoid its test patterns

---

## Next Steps (Implementation Phase)

1. Set up C test framework (Unity)
2. Create test file stubs
3. Set up TypeScript test framework (Vitest)
4. Create TypeScript test stubs
5. Write failing tests (test-first!)
6. Extract DiskANN code
7. Make tests pass
8. Add CI pipeline
