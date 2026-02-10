# Test-Driven Development for sqlite-ann

## Philosophy

**Write tests first.** Tests define the contract, drive design, and catch regressions. In C, where memory errors and undefined behavior lurk, tests are essential safety nets.

## The TDD Cycle

1. **Write a failing test** - Define what should work
2. **Write minimal code** - Make the test pass
3. **Refactor** - Clean up while tests stay green
4. **Repeat** - Next feature or edge case

## Test Framework

### Choice: Unity (or similar minimal C framework)

For sqlite-ann, we recommend [Unity](https://github.com/ThrowTheSwitch/Unity) - a lightweight, portable C testing framework.

```c
// Example test file: tests/test_vector_ops.c
#include "unity.h"
#include "../src/vector_ops.h"

void setUp(void) {
    // Runs before each test
}

void tearDown(void) {
    // Runs after each test
}

void test_euclidean_distance_simple(void) {
    float a[] = {1.0f, 0.0f};
    float b[] = {0.0f, 1.0f};
    float dist = euclidean_distance(a, b, 2);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.414f, dist);
}

void test_normalize_unit_vector(void) {
    float vec[] = {3.0f, 4.0f};
    int rc = normalize_vector(vec, 2);
    TEST_ASSERT_EQUAL(ANN_OK, rc);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, vec[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.8f, vec[1]);
}

void test_normalize_zero_vector(void) {
    float vec[] = {0.0f, 0.0f};
    int rc = normalize_vector(vec, 2);
    TEST_ASSERT_EQUAL(ANN_ERR_INVALID, rc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_euclidean_distance_simple);
    RUN_TEST(test_normalize_unit_vector);
    RUN_TEST(test_normalize_zero_vector);
    return UNITY_END();
}
```

### Alternative: Custom Minimal Framework

For maximum control, a simple custom framework:

```c
// tests/test_framework.h
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    printf("Running %s...", #name); \
    name(); \
    tests_run++; \
    printf(" PASS\n"); \
} while (0)

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "\nAssertion failed: %s\n  at %s:%d\n", \
                #expr, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(expected, actual) \
    ASSERT((expected) == (actual))

#define ASSERT_FLOAT_EQ(expected, actual, epsilon) \
    ASSERT(fabs((expected) - (actual)) < (epsilon))

#define TEST_SUMMARY() do { \
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed); \
    return tests_failed > 0 ? 1 : 0; \
} while (0)
```

## Test Organization

### Directory Structure

```
tests/
  test_framework.h       # Testing utilities (or Unity)
  test_vector_ops.c      # Unit tests for vector operations
  test_ann_core.c        # Unit tests for ANN algorithms
  test_index.c           # Tests for index management
  test_sqlite_ext.c      # Integration tests for SQLite extension
  fixtures/
    vectors_10k.bin      # Test data: 10K vectors
    queries_100.bin      # Test queries
  Makefile               # Build and run tests
```

### Test Categories

**Unit Tests** - Test individual functions in isolation

- Vector distance calculations
- Vector normalization
- Index data structure operations
- Memory allocation/deallocation

**Integration Tests** - Test components working together

- SQLite extension loading
- CREATE VIRTUAL TABLE
- INSERT INTO virtual table
- SELECT with ANN search

**Regression Tests** - Prevent known bugs from returning

- Segfaults with non-normalized vectors
- Memory leaks in cleanup paths
- Edge cases (zero dimension, empty index)

## Writing Good Tests

### Test One Thing

```c
// Good: focused test
void test_distance_euclidean_2d(void) {
    float a[] = {1.0f, 0.0f};
    float b[] = {0.0f, 1.0f};
    float dist = euclidean_distance(a, b, 2);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.414f, dist);
}

// Bad: tests multiple things
void test_distances(void) {
    // Tests Euclidean, cosine, dot product all in one
    // Hard to debug when it fails
}
```

### Test Error Paths

```c
void test_create_index_null_output(void) {
    int rc = ann_index_create(NULL, 128, ANN_METRIC_EUCLIDEAN);
    TEST_ASSERT_EQUAL(ANN_ERR_NULL, rc);
}

void test_create_index_zero_dimension(void) {
    ann_index_t *idx;
    int rc = ann_index_create(&idx, 0, ANN_METRIC_EUCLIDEAN);
    TEST_ASSERT_EQUAL(ANN_ERR_INVALID, rc);
}

void test_create_index_huge_dimension(void) {
    ann_index_t *idx;
    int rc = ann_index_create(&idx, ANN_MAX_DIMENSIONS + 1, ANN_METRIC_EUCLIDEAN);
    TEST_ASSERT_EQUAL(ANN_ERR_INVALID, rc);
}
```

### Test Memory Management

```c
void test_index_lifecycle(void) {
    ann_index_t *idx = NULL;

    // Create
    int rc = ann_index_create(&idx, 128, ANN_METRIC_EUCLIDEAN);
    TEST_ASSERT_EQUAL(ANN_OK, rc);
    TEST_ASSERT_NOT_NULL(idx);

    // Use
    float vec[128] = {0};
    rc = ann_index_insert(idx, vec);
    TEST_ASSERT_EQUAL(ANN_OK, rc);

    // Destroy
    ann_index_destroy(idx);
    // No leak check here - use Valgrind
}
```

## Test Data Management

### Small Fixed Data

```c
// Hardcoded test vectors
static const float TEST_VECTORS[][3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
};
```

### Large Generated Data

```c
// Helper to generate random test vectors
float *generate_random_vectors(size_t count, size_t dim, unsigned int seed) {
    srand(seed);
    float *vecs = malloc(count * dim * sizeof(float));
    if (!vecs) return NULL;
    for (size_t i = 0; i < count * dim; i++) {
        vecs[i] = (float)rand() / RAND_MAX;
    }
    return vecs;
}

void test_search_large_index(void) {
    float *vecs = generate_random_vectors(10000, 128, 42);
    TEST_ASSERT_NOT_NULL(vecs);
    // ... test with 10K vectors ...
    free(vecs);
}
```

### Binary Fixtures

```c
// Load test vectors from file
float *load_vectors_from_file(const char *path, size_t *count, size_t *dim) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fread(count, sizeof(size_t), 1, f);
    fread(dim, sizeof(size_t), 1, f);
    float *vecs = malloc(*count * *dim * sizeof(float));
    fread(vecs, sizeof(float), *count * *dim, f);
    fclose(f);
    return vecs;
}
```

## SQLite Extension Testing

### Test Extension Loading

```c
#include <sqlite3.h>

void test_extension_loads(void) {
    sqlite3 *db;
    int rc = sqlite3_open(":memory:", &db);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    rc = sqlite3_load_extension(db, "./ann.so", NULL, NULL);
    TEST_ASSERT_EQUAL(SQLITE_OK, rc);

    sqlite3_close(db);
}
```

### Test Virtual Table Creation

```c
void test_create_virtual_table(void) {
    sqlite3 *db;
    sqlite3_open(":memory:", &db);
    sqlite3_load_extension(db, "./ann.so", NULL, NULL);

    char *err = NULL;
    int rc = sqlite3_exec(db,
        "CREATE VIRTUAL TABLE embeddings USING ann(dimension=128)",
        NULL, NULL, &err);

    TEST_ASSERT_EQUAL(SQLITE_OK, rc);
    if (err) {
        printf("Error: %s\n", err);
        sqlite3_free(err);
    }

    sqlite3_close(db);
}
```

### Test Search Queries

```c
void test_ann_search_query(void) {
    sqlite3 *db;
    sqlite3_open(":memory:", &db);
    sqlite3_load_extension(db, "./ann.so", NULL, NULL);

    // Create and populate table
    sqlite3_exec(db, "CREATE VIRTUAL TABLE e USING ann(dimension=3)", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO e(id, vector) VALUES (1, '[1,0,0]')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO e(id, vector) VALUES (2, '[0,1,0]')", NULL, NULL, NULL);

    // Search
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, distance FROM e WHERE ann_search(vector, '[1,0.1,0]', 1)",
        -1, &stmt, NULL);

    TEST_ASSERT_EQUAL(SQLITE_OK, rc);
    rc = sqlite3_step(stmt);
    TEST_ASSERT_EQUAL(SQLITE_ROW, rc);

    int64_t id = sqlite3_column_int64(stmt, 0);
    TEST_ASSERT_EQUAL(1, id);  // Closest to [1,0,0]

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}
```

## Verification & Continuous Testing

### Makefile Integration

```makefile
.PHONY: test test-unit test-integration test-mem

test: test-unit test-integration

test-unit: tests/test_vector_ops tests/test_ann_core
	@echo "Running unit tests..."
	./tests/test_vector_ops
	./tests/test_ann_core

test-integration: tests/test_sqlite_ext ann.so
	@echo "Running integration tests..."
	./tests/test_sqlite_ext

tests/%: tests/%.c src/*.c
	$(CC) $(CFLAGS) -o $@ $^ -lsqlite3

test-mem: test
	@echo "Running tests under Valgrind..."
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_vector_ops
	valgrind --leak-check=full --error-exitcode=1 ./tests/test_ann_core

test-sanitize:
	@echo "Building with AddressSanitizer..."
	$(CC) $(CFLAGS) -fsanitize=address -g tests/test_*.c src/*.c -o test_asan
	./test_asan
	rm test_asan
```

### Pre-Commit Hook

```bash
#!/bin/bash
# .git/hooks/pre-commit
make test
if [ $? -ne 0 ]; then
    echo "Tests failed. Commit aborted."
    exit 1
fi
```

## Benchmarking

Include performance tests to catch regressions:

```c
#include <time.h>

void benchmark_search_10k_vectors(void) {
    // Setup: create index with 10K vectors
    ann_index_t *idx = create_populated_index(10000, 128);

    float query[128];
    generate_random_vector(query, 128);

    // Benchmark
    clock_t start = clock();
    for (int i = 0; i < 1000; i++) {
        ann_result_t results[10];
        ann_search(idx, query, 10, results);
    }
    clock_t end = clock();

    double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000.0;
    double qps = 1000.0 / (ms / 1000.0);

    printf("Search performance: %.2f QPS (%.2f ms per 1000 queries)\n", qps, ms);

    // Assert minimum performance
    TEST_ASSERT_TRUE(qps > 1000.0);  // At least 1000 queries/sec

    ann_index_destroy(idx);
}
```

## Coverage Analysis

Use `gcov` for code coverage:

```bash
# Compile with coverage flags
gcc -fprofile-arcs -ftest-coverage src/*.c tests/*.c -o test_suite

# Run tests
./test_suite

# Generate coverage report
gcov src/*.c
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

Aim for >80% coverage, 100% for critical paths (search, insert, memory management).

## Common Testing Patterns

### Setup/Teardown Pattern

```c
static ann_index_t *test_index = NULL;

void setUp(void) {
    ann_index_create(&test_index, 128, ANN_METRIC_EUCLIDEAN);
}

void tearDown(void) {
    if (test_index) {
        ann_index_destroy(test_index);
        test_index = NULL;
    }
}
```

### Parameterized Tests (Manual)

```c
typedef struct {
    const char *name;
    ann_metric_t metric;
    float expected_dist;
} distance_test_case;

void test_distance_metrics(void) {
    float a[] = {1.0f, 0.0f};
    float b[] = {0.0f, 1.0f};

    distance_test_case cases[] = {
        {"Euclidean", ANN_METRIC_EUCLIDEAN, 1.414f},
        {"Cosine", ANN_METRIC_COSINE, 1.0f},
        {"Dot", ANN_METRIC_DOT, 0.0f},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        float dist = compute_distance(a, b, 2, cases[i].metric);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, cases[i].expected_dist, dist);
    }
}
```

## When Tests Fail

### Debug Process

1. **Run single test**: Isolate the failing test
2. **Add printfs**: Debug intermediate values
3. **Valgrind**: Check for memory errors
4. **GDB**: Step through with debugger
5. **Simplify**: Reduce to minimal failing case

### Example Debug Session

```bash
# Run single test with verbose output
./test_suite --test=test_search_edge_case --verbose

# Run under Valgrind
valgrind --leak-check=full ./test_suite --test=test_search_edge_case

# Run under GDB
gdb --args ./test_suite --test=test_search_edge_case
(gdb) break ann_search
(gdb) run
(gdb) print *idx
```

## Test-First Examples

### Example 1: Vector Normalization

**1. Write failing test:**

```c
void test_normalize_vector(void) {
    float vec[] = {3.0f, 4.0f};
    int rc = normalize_vector(vec, 2);
    TEST_ASSERT_EQUAL(ANN_OK, rc);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.6f, vec[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.8f, vec[1]);
}
```

**2. Write minimal implementation:**

```c
int normalize_vector(float *vec, size_t dim) {
    if (!vec || dim == 0) return ANN_ERR_NULL;

    float norm = 0.0f;
    for (size_t i = 0; i < dim; i++) {
        norm += vec[i] * vec[i];
    }
    norm = sqrtf(norm);

    if (norm == 0.0f) return ANN_ERR_INVALID;

    for (size_t i = 0; i < dim; i++) {
        vec[i] /= norm;
    }

    return ANN_OK;
}
```

**3. Refactor:** Consider SIMD, edge cases, etc.

## Summary

- **Write tests first** - They define the contract
- **Test error paths** - NULL inputs, allocation failures
- **Use memory checkers** - Valgrind, ASan are essential
- **Keep tests fast** - Separate slow integration tests
- **Measure coverage** - Aim for >80%
- **Benchmark** - Catch performance regressions

**Remember:** In C, tests are your primary defense against memory errors, undefined behavior, and regressions. Invest in good tests early.
