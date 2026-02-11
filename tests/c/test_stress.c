/*
** Stress tests for DiskANN — performance benchmarking at scale
**
** These tests measure real-world performance characteristics:
** - Memory usage (RSS)
** - Disk usage (database file size)
** - Insert latency and throughput
** - Search latency and QPS (single-threaded and multi-threaded)
** - Recall quality
**
** Two main scenarios:
** 1. 300k vectors @ 92D (binary/quantized embeddings)
** 2. 100k vectors @ 256D (standard embeddings)
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/

/* Must be first - suppress MSVC warnings before any includes */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#define _POSIX_C_SOURCE 199309L
#include "unity/unity.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Platform-specific headers for file operations */
#ifdef _WIN32
#include <io.h>
#include <process.h> /* For _getpid */
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h> /* For QueryPerformanceCounter */
#define unlink _unlink
#define getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../../src/diskann.h"
#include "../../src/diskann_internal.h"
#include "../../src/diskann_node.h"

/**************************************************************************
** Configuration Constants
**************************************************************************/

/* Balance transaction overhead vs rollback cost */
#define DEFAULT_BATCH_SIZE 100

/* Prevent excessive memory per transaction */
#define MAX_BATCH_SIZE 10000

/* Report every 10% during insert phase */
#define PROGRESS_REPORT_COUNT 10

/* Number of queries for search benchmarking */
#define SEARCH_BENCHMARK_QUERIES 100

/* Number of queries for recall measurement (small sample for performance) */
#define RECALL_SAMPLE_QUERIES 20

/* k value for recall measurement */
#define RECALL_K 10

/**************************************************************************
** Platform detection for memory measurement
**************************************************************************/
#ifdef __linux__
#define HAS_PROC_STATUS 1
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#define HAS_MACH_TASK_INFO 1
#endif

/**************************************************************************
** Note: Parallel search support planned for future optimization.
** See _todo/20250210-parallel-graph-construction.md
**************************************************************************/

/**************************************************************************
** Global State for Cleanup
**************************************************************************/

static const char *g_temp_db_path_1 = NULL;
static const char *g_temp_db_path_2 = NULL;

static void cleanup_temp_files(void) {
  if (g_temp_db_path_1) {
    unlink(g_temp_db_path_1);
    /* Also remove WAL/SHM files */
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", g_temp_db_path_1);
    unlink(wal_path);
    snprintf(wal_path, sizeof(wal_path), "%s-shm", g_temp_db_path_1);
    unlink(wal_path);
  }
  if (g_temp_db_path_2) {
    unlink(g_temp_db_path_2);
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", g_temp_db_path_2);
    unlink(wal_path);
    snprintf(wal_path, sizeof(wal_path), "%s-shm", g_temp_db_path_2);
    unlink(wal_path);
  }
}

/**************************************************************************
** Measurement Utilities
**************************************************************************/

/*
** Get current time in milliseconds (monotonic)
*/
static double get_time_ms(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  static int initialized = 0;
  LARGE_INTEGER counter;

  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = 1;
  }

  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

/*
** Get memory usage in KB (platform-specific)
** Returns -1 if not available
*/
static long get_memory_usage_kb(void) {
#ifdef HAS_PROC_STATUS
  /* Linux: read from /proc/self/status */
  FILE *f = fopen("/proc/self/status", "r");
  if (!f)
    return -1;

  char line[256];
  long vmrss_kb = -1;

  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      /* Parse "VmRSS:    12345 kB" */
      char *p = line + 6;
      while (*p == ' ' || *p == '\t')
        p++;
      vmrss_kb = atol(p);
      break;
    }
  }

  fclose(f);
  return vmrss_kb;
#elif defined(HAS_MACH_TASK_INFO)
  /* macOS: use task_info() */
  struct mach_task_basic_info info;
  mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t kr =
      task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &size);
  if (kr != KERN_SUCCESS)
    return -1;
  /* resident_size is in bytes, convert to KB */
  return (long)(info.resident_size / 1024);
#else
  return -1; /* Not available on this platform */
#endif
}

/*
** Get file size in bytes
*/
static long get_file_size_bytes(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (long)st.st_size;
}

/*
** Format bytes as human-readable (KB, MB, GB)
*/
static void format_bytes(long bytes, char *buf, size_t buf_size) {
  if (bytes < 1024) {
    snprintf(buf, buf_size, "%ld B", bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, buf_size, "%.1f KB", (double)bytes / 1024.0);
  } else if (bytes < 1024 * 1024 * 1024) {
    snprintf(buf, buf_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else {
    snprintf(buf, buf_size, "%.2f GB",
             (double)bytes / (1024.0 * 1024.0 * 1024.0));
  }
}

/**************************************************************************
** Vector Generation (same as test_integration.c)
**************************************************************************/

/*
** Deterministic pseudo-random float in [0, 1) using a simple LCG.
*/
static float rand_float(uint32_t *seed) {
  *seed = (*seed) * 1103515245u + 12345u;
  return (float)(*seed & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}

/*
** Generate n_vectors of dim-dimensional random vectors.
** Caller must free the returned pointer.
** Returns NULL if allocation would overflow or fails.
*/
static float *gen_vectors(int n_vectors, int dim, uint32_t seed) {
  /* Check for overflow before allocation */
  if (n_vectors <= 0 || dim <= 0)
    return NULL;

  size_t count = (size_t)n_vectors;
  size_t dimensions = (size_t)dim;

  /* Check: count * dimensions would overflow? */
  if (count > SIZE_MAX / dimensions)
    return NULL;

  size_t total_elements = count * dimensions;

  /* Check: total_elements * sizeof(float) would overflow? */
  if (total_elements > SIZE_MAX / sizeof(float))
    return NULL;

  float *vecs = (float *)malloc(total_elements * sizeof(float));
  if (!vecs)
    return NULL;

  for (size_t i = 0; i < total_elements; i++) {
    vecs[i] = rand_float(&seed);
  }
  return vecs;
}

/*
** Brute-force k-NN: compute distances from query to all vectors,
** return the top-k IDs (1-based) and distances.
**
** Returns 0 on success, -1 on error (NULL inputs or allocation failure).
*/
static int brute_force_knn(const float *vectors, int n_vectors, int dim,
                           const float *query, int k, int64_t *out_ids,
                           float *out_distances) {
  /* Validate inputs */
  if (!vectors || !query || !out_ids || !out_distances || k <= 0 ||
      n_vectors <= 0 || dim <= 0) {
    return -1;
  }

  /* Allocate working memory */
  float *dists = (float *)malloc((size_t)n_vectors * sizeof(float));
  int *indices = (int *)malloc((size_t)n_vectors * sizeof(int));
  if (!dists || !indices) {
    free(dists);
    free(indices);
    return -1;
  }

  /* Compute all distances */
  for (int i = 0; i < n_vectors; i++) {
    dists[i] = diskann_distance_l2(query, vectors + (size_t)i * (size_t)dim,
                                   (uint32_t)dim);
    indices[i] = i;
  }

  /* Partial selection sort for top-k */
  for (int i = 0; i < k && i < n_vectors; i++) {
    int min_idx = i;
    for (int j = i + 1; j < n_vectors; j++) {
      if (dists[indices[j]] < dists[indices[min_idx]]) {
        min_idx = j;
      }
    }
    int tmp = indices[i];
    indices[i] = indices[min_idx];
    indices[min_idx] = tmp;
  }

  /* Output top-k */
  int n_results = k < n_vectors ? k : n_vectors;
  for (int i = 0; i < n_results; i++) {
    out_ids[i] = (int64_t)(indices[i] + 1); /* IDs are 1-based */
    out_distances[i] = dists[indices[i]];
  }

  free(dists);
  free(indices);
  return 0;
}

/**************************************************************************
** Stress Test Configuration and Metrics
**************************************************************************/

typedef struct {
  int n_vectors;
  int dimensions;
  uint32_t block_size;
  char db_path[512];
  const char *index_name;
} StressTestConfig;

typedef struct {
  /* Insert metrics */
  double insert_time_sec;
  int insert_errors;

  /* Memory metrics */
  long mem_before_kb;
  long mem_after_kb;

  /* Disk metrics */
  long disk_bytes;

  /* Search metrics (for k=1, 10, 100) */
  double search_latency_ms[3];
  double search_qps[3];
  int search_errors;

  /* Recall metric */
  float recall_percent;
} StressTestMetrics;

/**************************************************************************
** Stress Test Implementation
**************************************************************************/

/*
** Run a stress test with the given configuration
** Returns 0 on success, -1 on error
*/
static int run_stress_test(const StressTestConfig *cfg,
                           StressTestMetrics *metrics) {
  /* Validate configuration */
  if (!cfg || !metrics || cfg->n_vectors <= 0 || cfg->dimensions <= 0 ||
      !cfg->db_path[0] || !cfg->index_name) {
    TEST_FAIL_MESSAGE("Invalid stress test configuration");
    return -1;
  }

  /* Initialize metrics */
  memset(metrics, 0, sizeof(*metrics));
  metrics->mem_before_kb = -1;
  metrics->mem_after_kb = -1;
  metrics->disk_bytes = -1;

  printf("\n");
  printf("=================================================================\n");
  printf("=== Stress Test: %d vectors @ %dD ===\n", cfg->n_vectors,
         cfg->dimensions);
  printf("=================================================================\n");

  /* 1. Create database */
  unlink(cfg->db_path); /* Remove any existing file */
  sqlite3 *db = NULL;
  int rc = sqlite3_open(cfg->db_path, &db);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, rc, "Failed to create database");

  /* 2. Create index */
  DiskAnnConfig diskann_cfg = {.dimensions = (uint32_t)cfg->dimensions,
                               .metric = DISKANN_METRIC_EUCLIDEAN,
                               .max_neighbors = 16,
                               .search_list_size = 100,
                               .insert_list_size = 200,
                               .block_size = cfg->block_size};

  printf("\nConfig:\n");
  printf("  dimensions: %u\n", diskann_cfg.dimensions);
  printf("  max_neighbors: %u\n", diskann_cfg.max_neighbors);
  printf("  search_list_size: %u\n", diskann_cfg.search_list_size);
  printf("  insert_list_size: %u\n", diskann_cfg.insert_list_size);
  printf("  block_size: %u\n", diskann_cfg.block_size);

  rc = diskann_create_index(db, "main", cfg->index_name, &diskann_cfg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "Failed to create index");

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", cfg->index_name, &idx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "Failed to open index");
  TEST_ASSERT_NOT_NULL(idx);

  /* 3. Measure baseline */
  metrics->mem_before_kb = get_memory_usage_kb();
  char mem_str[64];

  printf("\n");
  printf("=== INSERT PHASE ===\n");
  if (metrics->mem_before_kb >= 0) {
    format_bytes(metrics->mem_before_kb * 1024, mem_str, sizeof(mem_str));
    printf("Memory (baseline): %s\n", mem_str);
  }

  /* 4. Generate all vectors */
  printf("Generating %d vectors...\n", cfg->n_vectors);
  float *vectors = gen_vectors(cfg->n_vectors, cfg->dimensions,
                               42 + (uint32_t)cfg->dimensions);
  TEST_ASSERT_NOT_NULL_MESSAGE(vectors, "Failed to generate vectors");

  /* 5. Insert all vectors with timing and batched transactions */
  printf("Inserting %d vectors...\n", cfg->n_vectors);

  /* Enable WAL mode for better write performance */
  char *err_msg = NULL;
  rc = sqlite3_exec(idx->db, "PRAGMA journal_mode=WAL", NULL, NULL, &err_msg);
  if (rc != SQLITE_OK) {
    printf("Warning: Could not enable WAL mode: %s\n",
           err_msg ? err_msg : "unknown error");
    sqlite3_free(err_msg);
  }

  double insert_start = get_time_ms();
  int report_interval = cfg->n_vectors / PROGRESS_REPORT_COUNT;
  if (report_interval == 0)
    report_interval = 1;

  /* Batch size: configurable via environment */
  int batch_size = DEFAULT_BATCH_SIZE;
  char *batch_env = getenv("STRESS_BATCH_SIZE");
  if (batch_env) {
    int n = atoi(batch_env);
    if (n > 0 && n <= MAX_BATCH_SIZE)
      batch_size = n;
  }

  printf("Using batched transactions (batch_size=%d)...\n", batch_size);

  for (int i = 0; i < cfg->n_vectors; i++) {
    /* Begin transaction for each batch */
    if (i % batch_size == 0) {
      rc = sqlite3_exec(idx->db, "BEGIN IMMEDIATE", NULL, NULL, &err_msg);
      if (rc != SQLITE_OK) {
        printf("Warning: BEGIN failed: %s\n",
               err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        metrics->insert_errors++;
      }
    }

    rc = diskann_insert(idx, (int64_t)(i + 1),
                        vectors + (size_t)i * (size_t)cfg->dimensions,
                        (uint32_t)cfg->dimensions);
    if (rc != DISKANN_OK) {
      metrics->insert_errors++;
      TEST_FAIL_MESSAGE("Insert failed");
      free(vectors);
      diskann_close_index(idx);
      sqlite3_close(db);
      return -1;
    }

    /* Commit at end of batch */
    if ((i + 1) % batch_size == 0 || (i + 1) == cfg->n_vectors) {
      rc = sqlite3_exec(idx->db, "COMMIT", NULL, NULL, &err_msg);
      if (rc != SQLITE_OK) {
        printf("Warning: COMMIT failed: %s\n",
               err_msg ? err_msg : "unknown error");
        sqlite3_free(err_msg);
        metrics->insert_errors++;
      }
    }

    if ((i + 1) % report_interval == 0) {
      double elapsed = get_time_ms() - insert_start;
      double rate = (double)(i + 1) / (elapsed / 1000.0);
      printf("  %d/%d vectors (%.0f vectors/sec)\n", i + 1, cfg->n_vectors,
             rate);
    }
  }

  double insert_end = get_time_ms();
  metrics->insert_time_sec = (insert_end - insert_start) / 1000.0;

  /* Close and reopen to flush to disk */
  diskann_close_index(idx);
  idx = NULL;

  rc = diskann_open_index(db, "main", cfg->index_name, &idx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "Failed to reopen index");
  TEST_ASSERT_NOT_NULL(idx);

  /* 6. Measure final state */
  metrics->mem_after_kb = get_memory_usage_kb();
  metrics->disk_bytes = get_file_size_bytes(cfg->db_path);

  printf("\nInsert Summary:\n");
  printf("  Total vectors: %d\n", cfg->n_vectors);
  printf("  Total time: %.2fs\n", metrics->insert_time_sec);
  printf("  Avg time/vector: %.3fms\n",
         (metrics->insert_time_sec * 1000.0) / (double)cfg->n_vectors);
  printf("  Throughput: %.0f vectors/sec\n",
         (double)cfg->n_vectors / metrics->insert_time_sec);
  printf("  Errors: %d\n", metrics->insert_errors);

  if (metrics->mem_before_kb >= 0 && metrics->mem_after_kb >= 0) {
    format_bytes(metrics->mem_before_kb * 1024, mem_str, sizeof(mem_str));
    printf("  Memory: %s → ", mem_str);
    format_bytes(metrics->mem_after_kb * 1024, mem_str, sizeof(mem_str));
    printf("%s", mem_str);
    long delta_kb = metrics->mem_after_kb - metrics->mem_before_kb;
    format_bytes(delta_kb * 1024, mem_str, sizeof(mem_str));
    printf(" (delta: %s)\n", mem_str);
  }

  if (metrics->disk_bytes >= 0) {
    format_bytes(metrics->disk_bytes, mem_str, sizeof(mem_str));
    printf("  Disk usage: %s\n", mem_str);
  }

  /* 7. Search benchmark */
  printf("\n");
  printf("=== SEARCH PHASE ===\n");

  float *search_queries =
      gen_vectors(SEARCH_BENCHMARK_QUERIES, cfg->dimensions,
                  99999 + (uint32_t)cfg->dimensions);
  TEST_ASSERT_NOT_NULL_MESSAGE(search_queries,
                               "Failed to generate search queries");

  int k_values[] = {1, 10, 100};
  int n_k_values = 3;

  printf("Running %d search queries for each k...\n", SEARCH_BENCHMARK_QUERIES);

  for (int k_idx = 0; k_idx < n_k_values; k_idx++) {
    int k = k_values[k_idx];
    double total_search_time = 0.0;

    for (int q = 0; q < SEARCH_BENCHMARK_QUERIES; q++) {
      float *query = search_queries + (size_t)q * (size_t)cfg->dimensions;

      /* Allocate results buffer */
      DiskAnnResult *results =
          (DiskAnnResult *)malloc((size_t)k * sizeof(DiskAnnResult));
      if (!results) {
        metrics->search_errors++;
        TEST_FAIL_MESSAGE("Failed to allocate search results");
        free(vectors);
        free(search_queries);
        diskann_close_index(idx);
        sqlite3_close(db);
        return -1;
      }

      double search_start = get_time_ms();
      int n = diskann_search(idx, query, (uint32_t)cfg->dimensions, k, results);
      double search_end = get_time_ms();

      if (n <= 0 && k <= cfg->n_vectors) {
        metrics->search_errors++;
      }

      total_search_time += (search_end - search_start);
      free(results);
    }

    metrics->search_latency_ms[k_idx] =
        total_search_time / (double)SEARCH_BENCHMARK_QUERIES;
    metrics->search_qps[k_idx] = 1000.0 / metrics->search_latency_ms[k_idx];

    printf("  k=%-3d  avg latency: %.3fms  (%.0f QPS)\n", k,
           metrics->search_latency_ms[k_idx], metrics->search_qps[k_idx]);
  }

  if (metrics->search_errors > 0) {
    printf("  Search errors: %d\n", metrics->search_errors);
  }

  /* 8. Recall measurement (sample-based for performance) */
  printf("\n");
  printf("=== RECALL MEASUREMENT ===\n");
  printf("Computing recall@%d over %d queries...\n", RECALL_K,
         RECALL_SAMPLE_QUERIES);

  int total_hits = 0;
  int total_possible = 0;

  for (int q = 0; q < RECALL_SAMPLE_QUERIES; q++) {
    float *query = search_queries + (size_t)q * (size_t)cfg->dimensions;

    /* Brute-force reference */
    int64_t bf_ids[RECALL_K];
    float bf_dists[RECALL_K];
    int bf_rc = brute_force_knn(vectors, cfg->n_vectors, cfg->dimensions, query,
                                RECALL_K, bf_ids, bf_dists);
    if (bf_rc != 0) {
      printf("Warning: Brute-force KNN failed for query %d\n", q);
      continue;
    }

    /* ANN search */
    DiskAnnResult ann_results[RECALL_K];
    int n = diskann_search(idx, query, (uint32_t)cfg->dimensions, RECALL_K,
                           ann_results);
    if (n <= 0) {
      printf("Warning: ANN search failed for query %d\n", q);
      continue;
    }

    /* Count hits */
    int actual_k = RECALL_K < n ? RECALL_K : n;
    for (int i = 0; i < actual_k; i++) {
      for (int j = 0; j < n; j++) {
        if (bf_ids[i] == ann_results[j].id) {
          total_hits++;
          break;
        }
      }
    }
    total_possible += actual_k;
  }

  metrics->recall_percent =
      total_possible > 0 ? (float)total_hits * 100.0f / (float)total_possible
                         : 0.0f;
  printf("  Recall@%d: %.1f%%\n", RECALL_K, (double)metrics->recall_percent);

  /* 9. Cleanup */
  free(vectors);
  free(search_queries);
  diskann_close_index(idx);
  sqlite3_close(db);

  printf("\n");
  printf("=================================================================\n");
  printf("\n");

  return 0;
}

/**************************************************************************
** Portable temporary file path generation
**************************************************************************/

static void get_temp_db_path(char *buf, size_t buf_size, const char *test_name,
                             const char **out_ptr) {
  const char *temp_dir = sqlite3_temp_directory;
  if (!temp_dir || !temp_dir[0]) {
#ifdef _WIN32
    temp_dir = getenv("TEMP");
    if (!temp_dir)
      temp_dir = getenv("TMP");
    if (!temp_dir)
      temp_dir = "C:\\TEMP";
#else
    temp_dir = "/tmp";
#endif
  }

  snprintf(buf, buf_size, "%s/diskann_stress_%s_%d.db", temp_dir, test_name,
           (int)getpid());
  *out_ptr = buf;
}

/**************************************************************************
** Stress Test 1: 300k vectors @ 92D
**************************************************************************/

void test_stress_300k_92d(void) {
  StressTestConfig cfg = {.n_vectors = 300000,
                          .dimensions = 92,
                          .block_size = 16384,
                          .index_name = "stress_300k_92d"};
  get_temp_db_path(cfg.db_path, sizeof(cfg.db_path), "300k_92d",
                   &g_temp_db_path_1);

  StressTestMetrics metrics;
  int rc = run_stress_test(&cfg, &metrics);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "Stress test failed");

  /* Verify basic quality thresholds */
  TEST_ASSERT_TRUE_MESSAGE(metrics.insert_errors == 0,
                           "Insert errors detected");
  TEST_ASSERT_TRUE_MESSAGE(metrics.search_errors == 0,
                           "Search errors detected");
  TEST_ASSERT_TRUE_MESSAGE(metrics.recall_percent > 80.0f,
                           "Recall below 80%");

  /* Cleanup database files */
  cleanup_temp_files();
}

/**************************************************************************
** Stress Test 2: 100k vectors @ 256D
**************************************************************************/

void test_stress_100k_256d(void) {
  StressTestConfig cfg = {.n_vectors = 100000,
                          .dimensions = 256,
                          .block_size = 32768,
                          .index_name = "stress_100k_256d"};
  get_temp_db_path(cfg.db_path, sizeof(cfg.db_path), "100k_256d",
                   &g_temp_db_path_2);

  StressTestMetrics metrics;
  int rc = run_stress_test(&cfg, &metrics);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "Stress test failed");

  /* Verify basic quality thresholds */
  TEST_ASSERT_TRUE_MESSAGE(metrics.insert_errors == 0,
                           "Insert errors detected");
  TEST_ASSERT_TRUE_MESSAGE(metrics.search_errors == 0,
                           "Search errors detected");
  TEST_ASSERT_TRUE_MESSAGE(metrics.recall_percent > 80.0f,
                           "Recall below 80%");

  /* Cleanup database files */
  cleanup_temp_files();
}

/**************************************************************************
** Main
**************************************************************************/

void setUp(void) {}
void tearDown(void) {}

int main(void) {
  /* Register cleanup handler for abnormal termination */
  atexit(cleanup_temp_files);

  UNITY_BEGIN();

  printf("\n");
  printf("====================================================\n");
  printf("DiskANN Stress Tests\n");
  printf("====================================================\n");
  printf("These tests measure performance at scale and may take\n");
  printf("several minutes to complete.\n");
  printf("\n");

#if !defined(HAS_PROC_STATUS) && !defined(HAS_MACH_TASK_INFO)
  printf("WARNING: Memory measurement not available on this platform\n");
  printf("         (requires Linux /proc/self/status or macOS task_info)\n");
  printf("\n");
#endif

  RUN_TEST(test_stress_300k_92d);
  RUN_TEST(test_stress_100k_256d);

  return UNITY_END();
}
