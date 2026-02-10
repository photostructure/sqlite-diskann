# sqlite-ann Project Guidelines

## Project Overview

sqlite-ann is a SQLite extension providing Approximate Nearest Neighbor (ANN) search capabilities for vector embeddings.

**Language:** C (C11+) with TypeScript/Node.js bindings
**Target:** SQLite extension with high performance and portability
**Core Focus:** Vector similarity search with ANN algorithms
**Package Format:** Hybrid CJS/ESM for maximum compatibility

## Reference Projects

When setting up build infrastructure, tooling, and CI/CD pipelines, use these sibling projects as references:

### ✅ DO follow: `../fs-metadata`

- **Use for:** Cross-platform GHA (GitHub Actions) build pipeline
- **Why:** Has a well-designed, clean cross-platform build setup that we want to match
- **What to reference:**
  - GitHub Actions workflows, cross-platform compilation, build matrix setup
  - AddressSanitizer (asan) configuration
  - Bear (Build EAR - compilation database generator) setup
  - clang-tidy integration and configuration

### ✅ Also reference: `../node-sqlite`

- **Use for:** Additional tooling examples
- **What to reference:**
  - AddressSanitizer (asan) setup
  - Bear configuration
  - clang-tidy configuration

### ❌ DO NOT follow: `../sqlite-vec`

- **Avoid:** The npm pipeline implementation
- **Why:** It was bolted on after a fork and doesn't represent good design
- **Note:** Other aspects of sqlite-vec may still be useful for SQLite extension patterns

When in doubt about build setup, packaging, CI/CD configuration, or static analysis tooling, check fs-metadata and node-sqlite first.

## Copyright and Licensing

**CRITICAL: Preserve original copyright notices**

This project is derived from libSQL's DiskANN implementation (MIT licensed). All derived code MUST:

- Retain libSQL's original copyright: `Copyright 2024 the libSQL authors`
- Add our modifications copyright: `Modifications Copyright 2025 PhotoStructure Inc.`
- Keep the MIT license text intact
- NEVER claim sole copyright on derived code

**For new files (not derived from libSQL):**

```c
/*
** Copyright 2025 PhotoStructure Inc.
** MIT License (see LICENSE file)
*/
```

**For derived/modified files:**

```c
/*
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
** MIT License (see LICENSE file)
*/
```

## Required Reading

Before making ANY changes, you MUST read:

1. @DESIGN-PRINCIPLES.md - C coding standards and best practices
2. @TDD.md - Testing conventions and methodology
3. Relevant source files for the area you're working on

## Code Style & Conventions

### C Standards

- Use C17 standard (C11 minimum)
- Follow all conventions in @DESIGN-PRINCIPLES.md
- Compile with `-Wall -Wextra -Werror -pedantic`

### Naming Conventions

```c
// Functions: snake_case with prefix
int ann_index_create(ann_index_t **out);
int ann_search(ann_index_t *idx, float *query, int k, ann_result_t *results);

// Types: snake_case with _t suffix
typedef struct ann_index ann_index_t;
typedef struct ann_result ann_result_t;

// Constants: SCREAMING_SNAKE_CASE with prefix
#define ANN_MAX_DIMENSIONS 2048
#define ANN_DEFAULT_K 10

// Static (internal) functions: snake_case without prefix
static int compute_distance(const float *a, const float *b, size_t dim);
```

### File Organization

```
src/
  ann_core.c          # Core ANN algorithm implementation
  ann_core.h          # Public API
  ann_sqlite.c        # SQLite extension interface
  ann_index.c         # Index management
  utils/
    vector_ops.c      # Vector operations (distance, normalization)
    vector_ops.h
tests/
  test_ann_core.c     # Core algorithm tests
  test_vector_ops.c   # Vector operation tests
  test_integration.c  # SQLite integration tests
```

### Memory Management

- Follow ownership model from @DESIGN-PRINCIPLES.md
- All public APIs validate inputs
- Use `goto cleanup` pattern for resource cleanup
- Null pointers after free: `free(ptr); ptr = NULL;`

### Error Handling

```c
// Return codes
#define ANN_OK 0
#define ANN_ERR_NULL -1
#define ANN_ERR_NOMEM -2
#define ANN_ERR_INVALID -3
#define ANN_ERR_IO -4
#define ANN_ERR_SQLITE -5

// Always check allocations
float *vectors = malloc(count * dim * sizeof(float));
if (!vectors) return ANN_ERR_NOMEM;

// Never mask errors with defaults
// Bad: int get_dimension(ann_index_t *idx) { return idx ? idx->dim : 0; }
// Good: int get_dimension(ann_index_t *idx, size_t *dim) {
//         if (!idx || !dim) return ANN_ERR_NULL;
//         *dim = idx->dim; return ANN_OK;
//       }
```

## SQLite Extension Conventions

### Loading Pattern

```c
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_ann_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  SQLITE_EXTENSION_INIT2(pApi);
  // Register virtual tables, functions, etc.
  return SQLITE_OK;
}
```

### Virtual Table Pattern

- Use SQLite's virtual table interface for ANN indexes
- Implement `xCreate`, `xConnect`, `xBestIndex`, `xFilter`, etc.
- Document which SQLite versions are supported

### SQL Function Registration

```c
// Register ANN search function
sqlite3_create_function_v2(
  db, "ann_search", 3, SQLITE_UTF8, NULL,
  ann_search_func, NULL, NULL, NULL
);
```

## Testing Requirements

### Unit Tests

- Test all vector operations (distance, normalization)
- Test ANN algorithms with known datasets
- Test error paths (NULL inputs, allocation failures)

### Integration Tests

- Test SQLite extension loading
- Test CREATE VIRTUAL TABLE
- Test search queries with various parameters
- Test concurrent access if supported

### Verification Commands

```bash
# Compile with all warnings
gcc -std=c17 -Wall -Wextra -Werror -pedantic -Wconversion \
    -Wshadow -Wstrict-prototypes -shared -fPIC \
    -o ann.so src/*.c -lsqlite3

# Run tests
./test_suite

# Memory check
valgrind --leak-check=full ./test_suite

# Address sanitizer
gcc -fsanitize=address -g src/*.c tests/*.c -o test_suite
./test_suite
```

## Performance Considerations

### Vector Operations

- Use SIMD when available (SSE, AVX, NEON)
- Minimize allocations in hot paths
- Consider cache-friendly data layouts

### Index Structure

- Balance memory usage vs search speed
- Support incremental index updates when possible
- Document time/space complexity

### Benchmarking

- Include benchmark suite for common operations
- Test with realistic dataset sizes (10K, 100K, 1M vectors)
- Measure queries per second and recall@k

## Documentation

### Code Comments

- Comment WHY, not WHAT
- Document ownership and lifecycle of resources
- Explain non-obvious algorithms or optimizations
- Keep comments up-to-date (no "lava flow")

### API Documentation

```c
/**
 * Create a new ANN index.
 *
 * @param out    Pointer to receive the new index (must not be NULL)
 * @param dim    Vector dimensionality (must be > 0 and <= ANN_MAX_DIMENSIONS)
 * @param metric Distance metric (e.g., ANN_METRIC_EUCLIDEAN)
 * @return       ANN_OK on success, error code on failure
 *
 * The caller takes ownership of the returned index and must call
 * ann_index_destroy() when done.
 *
 * Example:
 *   ann_index_t *idx;
 *   int rc = ann_index_create(&idx, 128, ANN_METRIC_EUCLIDEAN);
 *   if (rc != ANN_OK) { /* handle error */ }
 *   // ... use index ...
 *   ann_index_destroy(idx);
 */
int ann_index_create(ann_index_t **out, size_t dim, ann_metric_t metric);
```

## Git Workflow

### Commits

- Follow Conventional Commits format
- Scope is the most-changed file (without extension)
- Keep commits focused and atomic
- DO NOT include Co-Authored-By tags

Example:

```
feat(ann_core): implement HNSW algorithm for ANN search

Adds hierarchical navigable small world graph implementation
with configurable M and efConstruction parameters.
Benchmarks show 10x speedup vs brute force on 100K vectors.
```

### Branches

- Feature branches: `feature/hnsw-index`
- Bug fixes: `fix/normalize-vectors`
- Experiments: `exp/simd-distance`

## Don't Guess - Verify

- Always check function signatures in SQLite headers
- Verify vector math formulas (Euclidean, cosine, dot product)
- Test edge cases (zero vectors, high dimensions, large datasets)
- Use static analyzers and sanitizers to catch issues early

## Build System

Use a simple Makefile or CMake:

```makefile
# Example Makefile snippet
CFLAGS = -std=c17 -Wall -Wextra -Werror -pedantic -fPIC
LDFLAGS = -shared -lsqlite3

ann.so: src/*.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: tests/*.c src/*.c
	$(CC) $(CFLAGS) -o test_suite $^ -lsqlite3
	./test_suite

clean:
	rm -f ann.so test_suite *.o
```

## TypeScript/npm Package Requirements

### Hybrid CJS/ESM Support

**CRITICAL: This library must support both CommonJS and ESM consumers.**

The package.json must be configured for dual-format distribution:

```json
{
  "type": "module",
  "main": "./dist/index.cjs",
  "module": "./dist/index.mjs",
  "types": "./dist/index.d.ts",
  "exports": {
    ".": {
      "require": {
        "types": "./dist/index.d.cts",
        "default": "./dist/index.cjs"
      },
      "import": {
        "types": "./dist/index.d.ts",
        "default": "./dist/index.mjs"
      }
    },
    "./package.json": "./package.json"
  }
}
```

**Why this matters:**

- Node.js projects may use `require()` (CJS) or `import` (ESM)
- TypeScript consumers need correct `.d.ts` vs `.d.cts` types
- Bundlers (webpack, vite, rollup) need to understand the export map
- Without proper configuration, imports will fail at runtime

**Reference:** See `../fs-metadata/package.json` for the canonical example.

### SQL Injection Prevention

**TypeScript wrapper functions MUST validate all SQL identifiers before interpolation.**

```typescript
// Bad - SQL injection vulnerability
function createIndex(db: Database, tableName: string) {
  db.exec(`CREATE VIRTUAL TABLE ${tableName} USING diskann(...)`);
}

// Good - validate identifier first
function createIndex(db: Database, tableName: string) {
  if (!isValidIdentifier(tableName)) {
    throw new Error(`Invalid table name: ${tableName}`);
  }
  db.exec(`CREATE VIRTUAL TABLE ${tableName} USING diskann(...)`);
}

function isValidIdentifier(name: string): boolean {
  // Match C layer validate_identifier logic
  return /^[a-zA-Z_][a-zA-Z0-9_]*$/.test(name) && name.length <= 64;
}
```

All wrapper functions (createDiskAnnIndex, searchNearest, insertVector, deleteVector) that interpolate table/column names must validate identifiers to prevent injection attacks like `"; DROP TABLE users; --"`.

### Package Distribution

**Include in npm package:**

- `build/` - Prebuilt native binaries for supported platforms
- `src/` - TypeScript source for sourcemaps and debugging
- `dist/` - Compiled CJS/ESM JavaScript and type definitions
- `README.md`, `LICENSE`

**Exclude from npm package:**

- Development docs (CLAUDE.md, DESIGN-PRINCIPLES.md, TDD.md)
- Tests (except maybe a smoke test)
- Build artifacts (compile_commands.json, \*.o files)

The `files` array in package.json controls what gets published. Be conservative - users don't need build infrastructure.

## Security Considerations

- Validate all SQL inputs to prevent injection
- Bounds-check all vector operations
- Prevent integer overflows in dimension calculations
- Use secure random for any randomized algorithms
- Document any known limitations or attack vectors
