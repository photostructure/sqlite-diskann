/*
** Insert profiling tests for DiskANN
**
** These tests measure per-phase timing of incremental inserts at scale.
** They are separated from the main test suite because they take minutes
** to complete (building 10k+ vector indexes).
**
** Run with DISKANN_DEBUG_TIMING=1 to emit per-insert phase timing CSV
** to stderr:
**   DISKANN_DEBUG_TIMING=1 ./build/test_profiling 2>timing.csv
**
** Two scenarios:
** 1. Random vectors: 500 incremental inserts into a pre-existing 10k index
** 2. Similar vectors: 500 clustered inserts to measure neighbor overlap
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/

/* Must be first - clock_gettime requires _POSIX_C_SOURCE on Linux */
#define _POSIX_C_SOURCE 199309L
#include "unity/unity.h"
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Platform-specific headers */
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#define unlink _unlink
#define getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../../src/diskann.h"
#include "../../src/diskann_node.h"

/**************************************************************************
** Configuration
**************************************************************************/

#define PROF_DIMS 128
#define PROF_BLOCK_SIZE 16384
#define PROF_MAX_NEIGHBORS 16
#define PROF_SEARCH_L 64
#define PROF_INSERT_L 128

/* Base index size — how many vectors to build before profiling */
#define PROF_BASE_SIZE 25000

/* Number of incremental inserts to profile */
#define PROF_INCREMENTAL_COUNT 500

/* Batch size for base index construction (transaction batching) */
#define PROF_BATCH_SIZE 100

/* Number of search queries to verify recall after inserts */
#define PROF_VERIFY_QUERIES 10
#define PROF_VERIFY_K 10

/**************************************************************************
** Helpers
**************************************************************************/

static const char *g_temp_db_path = NULL;

static void cleanup_temp_files(void) {
  if (g_temp_db_path) {
    unlink(g_temp_db_path);
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", g_temp_db_path);
    unlink(wal_path);
    snprintf(wal_path, sizeof(wal_path), "%s-shm", g_temp_db_path);
    unlink(wal_path);
  }
}

static void get_temp_db_path(char *buf, size_t buf_size, const char *test_name) {
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
  snprintf(buf, buf_size, "%s/diskann_profile_%s_%d.db", temp_dir, test_name,
           (int)getpid());
  g_temp_db_path = buf;
}

/*
** Deterministic pseudo-random float in [0, 1) using a simple LCG.
*/
static float rand_float(uint32_t *seed) {
  *seed = (*seed) * 1103515245u + 12345u;
  return (float)(*seed & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}

/*
** Generate n_vectors of PROF_DIMS-dimensional random vectors.
** Caller must free the returned pointer.
*/
static float *gen_random_vectors(int n_vectors, uint32_t seed) {
  if (n_vectors <= 0)
    return NULL;

  size_t total = (size_t)n_vectors * PROF_DIMS;
  float *vecs = (float *)malloc(total * sizeof(float));
  if (!vecs)
    return NULL;

  for (size_t i = 0; i < total; i++) {
    vecs[i] = rand_float(&seed);
  }
  return vecs;
}

/*
** Generate n_vectors of PROF_DIMS-dimensional vectors clustered around
** a center point. Each vector is center + noise, where noise is small
** relative to the center's magnitude.
**
** This simulates inserting a batch of similar embeddings (e.g., photos
** from the same event or near-duplicate text embeddings).
*/
static float *gen_similar_vectors(int n_vectors, uint32_t seed) {
  if (n_vectors <= 0)
    return NULL;

  size_t total = (size_t)n_vectors * PROF_DIMS;
  float *vecs = (float *)malloc(total * sizeof(float));
  if (!vecs)
    return NULL;

  /* Generate center point */
  float center[PROF_DIMS];
  for (int d = 0; d < PROF_DIMS; d++) {
    center[d] = rand_float(&seed);
  }

  /* Generate vectors as center + small noise (0.01 * random) */
  for (int i = 0; i < n_vectors; i++) {
    for (int d = 0; d < PROF_DIMS; d++) {
      float noise = (rand_float(&seed) - 0.5f) * 0.02f;
      vecs[(size_t)i * PROF_DIMS + (size_t)d] = center[d] + noise;
    }
  }

  return vecs;
}

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
** Build a base index with n_base vectors, returning the open index.
** Uses batched transactions for speed.
*/
static DiskAnnIndex *build_base_index(sqlite3 *db, const char *name,
                                       const float *vectors, int n_base) {
  DiskAnnConfig cfg = {.dimensions = PROF_DIMS,
                       .metric = DISKANN_METRIC_EUCLIDEAN,
                       .max_neighbors = PROF_MAX_NEIGHBORS,
                       .search_list_size = PROF_SEARCH_L,
                       .insert_list_size = PROF_INSERT_L,
                       .block_size = PROF_BLOCK_SIZE};

  int rc = diskann_create_index(db, "main", name, &cfg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "create_index failed");

  DiskAnnIndex *idx = NULL;
  rc = diskann_open_index(db, "main", name, &idx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "open_index failed");
  TEST_ASSERT_NOT_NULL(idx);

  /* Enable WAL mode */
  sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);

  printf("  Building base index: %d vectors @ %dD...\n", n_base, PROF_DIMS);

  double start = get_time_ms();
  int report_interval = n_base / 10;
  if (report_interval == 0)
    report_interval = 1;

  for (int i = 0; i < n_base; i++) {
    /* Transaction batching */
    if (i % PROF_BATCH_SIZE == 0) {
      sqlite3_exec(idx->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    }

    rc = diskann_insert(idx, (int64_t)(i + 1),
                        vectors + (size_t)i * PROF_DIMS, PROF_DIMS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISKANN_OK, rc, "base insert failed");

    if ((i + 1) % PROF_BATCH_SIZE == 0 || (i + 1) == n_base) {
      sqlite3_exec(idx->db, "COMMIT", NULL, NULL, NULL);
    }

    if ((i + 1) % report_interval == 0) {
      double elapsed = get_time_ms() - start;
      double rate = (double)(i + 1) / (elapsed / 1000.0);
      printf("    %d/%d (%.0f vec/sec)\n", i + 1, n_base, rate);
    }
  }

  double elapsed = get_time_ms() - start;
  printf("  Base index built: %.1fs (%.0f vec/sec)\n", elapsed / 1000.0,
         (double)n_base / (elapsed / 1000.0));

  return idx;
}

/*
** Brute-force k-NN for recall verification.
*/
static void brute_force_knn(const float *vectors, int n_vectors,
                             const float *query, int k, int64_t *out_ids) {
  float *dists = (float *)malloc((size_t)n_vectors * sizeof(float));
  int *indices = (int *)malloc((size_t)n_vectors * sizeof(int));
  TEST_ASSERT_NOT_NULL(dists);
  TEST_ASSERT_NOT_NULL(indices);

  for (int i = 0; i < n_vectors; i++) {
    dists[i] = diskann_distance_l2(query, vectors + (size_t)i * PROF_DIMS,
                                   PROF_DIMS);
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

  int n_results = k < n_vectors ? k : n_vectors;
  for (int i = 0; i < n_results; i++) {
    out_ids[i] = (int64_t)(indices[i] + 1); /* IDs are 1-based */
  }

  free(dists);
  free(indices);
}

/**************************************************************************
** Test 1: Incremental random inserts into pre-existing 10k index
**
** This is the primary profiling test. It builds a 10k index, then inserts
** 500 more random vectors. With DISKANN_DEBUG_TIMING=1, each incremental
** insert emits a timing CSV line to stderr with per-phase breakdown.
**
** Expected output fields (when timing enabled):
**   id, total_us, random_start_us, savepoint_us, search_us, shadow_row_us,
**   phase1_us, phase2_us, flush_new_us, cleanup_us,
**   cache_hits, cache_misses, visited_count, phase2_flushes
**
** Assertions:
** - All 500 incremental inserts succeed
** - Search still works after inserts (recall >= 60%)
** - Timing CSV has 500 lines (when enabled)
**************************************************************************/

void test_profile_incremental_10k(void) {
  char db_path[512];
  get_temp_db_path(db_path, sizeof(db_path), "incr_10k");

  /* Remove any existing file */
  unlink(db_path);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(db_path, &db);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, rc, "open database failed");

  printf("\n");
  printf("=================================================================\n");
  printf("=== Profile: Incremental Random Inserts (10k base + 500)      ===\n");
  printf("=================================================================\n");

  /* Generate all vectors upfront (base + incremental) */
  int total_vectors = PROF_BASE_SIZE + PROF_INCREMENTAL_COUNT;
  float *all_vectors = gen_random_vectors(total_vectors, 42);
  TEST_ASSERT_NOT_NULL(all_vectors);

  /* Build base index with first 10k vectors */
  DiskAnnIndex *idx = build_base_index(db, "prof_incr", all_vectors,
                                        PROF_BASE_SIZE);

  /* Now insert 500 more vectors — these are the ones being profiled */
  printf("\n  Profiling %d incremental inserts...\n", PROF_INCREMENTAL_COUNT);
  printf("  (Set DISKANN_DEBUG_TIMING=1 for per-insert phase timing)\n");

  double incr_start = get_time_ms();
  int insert_errors = 0;

  for (int i = 0; i < PROF_INCREMENTAL_COUNT; i++) {
    int vec_idx = PROF_BASE_SIZE + i;
    int64_t id = (int64_t)(vec_idx + 1);

    rc = diskann_insert(idx, id,
                        all_vectors + (size_t)vec_idx * PROF_DIMS, PROF_DIMS);
    if (rc != DISKANN_OK) {
      insert_errors++;
    }

    /* Progress every 100 inserts */
    if ((i + 1) % 100 == 0) {
      double elapsed = get_time_ms() - incr_start;
      double avg_ms = elapsed / (double)(i + 1);
      printf("    %d/%d inserts (avg %.2f ms/insert)\n", i + 1,
             PROF_INCREMENTAL_COUNT, avg_ms);
    }
  }

  double incr_elapsed = get_time_ms() - incr_start;
  printf("\n  Incremental insert summary:\n");
  printf("    Count: %d\n", PROF_INCREMENTAL_COUNT);
  printf("    Errors: %d\n", insert_errors);
  printf("    Total: %.1f ms\n", incr_elapsed);
  printf("    Avg: %.2f ms/insert\n", incr_elapsed / PROF_INCREMENTAL_COUNT);
  printf("    Throughput: %.0f inserts/sec\n",
         (double)PROF_INCREMENTAL_COUNT / (incr_elapsed / 1000.0));

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, insert_errors,
                                "Incremental inserts had errors");

  /* Verify search still works after incremental inserts */
  printf("\n  Verifying search quality...\n");

  float *queries = gen_random_vectors(PROF_VERIFY_QUERIES, 99999);
  TEST_ASSERT_NOT_NULL(queries);

  int total_hits = 0;
  int total_possible = 0;

  for (int q = 0; q < PROF_VERIFY_QUERIES; q++) {
    float *query = queries + (size_t)q * PROF_DIMS;

    /* Brute-force reference */
    int64_t bf_ids[PROF_VERIFY_K];
    brute_force_knn(all_vectors, total_vectors, query, PROF_VERIFY_K, bf_ids);

    /* ANN search */
    DiskAnnResult ann_results[PROF_VERIFY_K];
    int n = diskann_search(idx, query, PROF_DIMS, PROF_VERIFY_K, ann_results);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "search returned 0 results");

    /* Count hits */
    int actual_k = PROF_VERIFY_K < n ? PROF_VERIFY_K : n;
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

  float recall = (float)total_hits / (float)total_possible;
  printf("    Recall@%d: %.1f%% (%d/%d hits)\n", PROF_VERIFY_K,
         (double)recall * 100.0, total_hits, total_possible);

  /* Recall >= 60% is a lenient threshold for 10k random vectors */
  char msg[128];
  snprintf(msg, sizeof(msg), "recall@%d = %.1f%% (expected >= 60%%)",
           PROF_VERIFY_K, (double)recall * 100.0);
  TEST_ASSERT_TRUE_MESSAGE(recall >= 0.6f, msg);

  printf("\n=================================================================\n");

  /* Cleanup */
  diskann_close_index(idx);
  free(all_vectors);
  free(queries);
  sqlite3_close(db);
  cleanup_temp_files();
}

/**************************************************************************
** Test 2: Clustered/similar vector inserts
**
** This test builds the same 10k base index, then inserts 500 vectors
** that are clustered around a single center point. This simulates
** inserting a batch of similar embeddings.
**
** Key metric: When these similar vectors share neighbors, Phase 2
** (back-edge updates) should see the same nodes repeatedly, making
** cache hits valuable. By comparing visited_count and phase2_flushes
** to the random test, we can quantify the caching opportunity.
**
** Assertions:
** - All 500 inserts succeed
** - Search works on the augmented index
**************************************************************************/

void test_profile_similar_vectors(void) {
  char db_path[512];
  get_temp_db_path(db_path, sizeof(db_path), "similar");

  /* Remove any existing file */
  unlink(db_path);

  sqlite3 *db = NULL;
  int rc = sqlite3_open(db_path, &db);
  TEST_ASSERT_EQUAL_INT_MESSAGE(SQLITE_OK, rc, "open database failed");

  printf("\n");
  printf("=================================================================\n");
  printf("=== Profile: Similar Vector Inserts (10k base + 500 cluster)  ===\n");
  printf("=================================================================\n");

  /* Generate base vectors (random) */
  float *base_vectors = gen_random_vectors(PROF_BASE_SIZE, 42);
  TEST_ASSERT_NOT_NULL(base_vectors);

  /* Build base index */
  DiskAnnIndex *idx = build_base_index(db, "prof_similar", base_vectors,
                                        PROF_BASE_SIZE);

  /* Generate 500 similar/clustered vectors */
  float *similar_vectors = gen_similar_vectors(PROF_INCREMENTAL_COUNT, 77777);
  TEST_ASSERT_NOT_NULL(similar_vectors);

  printf("\n  Profiling %d similar vector inserts...\n",
         PROF_INCREMENTAL_COUNT);
  printf("  (Set DISKANN_DEBUG_TIMING=1 for per-insert phase timing)\n");
  printf("  (Compare phase2_flushes to random test for cache opportunity)\n");

  double incr_start = get_time_ms();
  int insert_errors = 0;

  for (int i = 0; i < PROF_INCREMENTAL_COUNT; i++) {
    int64_t id = (int64_t)(PROF_BASE_SIZE + i + 1);

    rc = diskann_insert(idx, id,
                        similar_vectors + (size_t)i * PROF_DIMS, PROF_DIMS);
    if (rc != DISKANN_OK) {
      insert_errors++;
    }

    if ((i + 1) % 100 == 0) {
      double elapsed = get_time_ms() - incr_start;
      double avg_ms = elapsed / (double)(i + 1);
      printf("    %d/%d inserts (avg %.2f ms/insert)\n", i + 1,
             PROF_INCREMENTAL_COUNT, avg_ms);
    }
  }

  double incr_elapsed = get_time_ms() - incr_start;
  printf("\n  Similar vector insert summary:\n");
  printf("    Count: %d\n", PROF_INCREMENTAL_COUNT);
  printf("    Errors: %d\n", insert_errors);
  printf("    Total: %.1f ms\n", incr_elapsed);
  printf("    Avg: %.2f ms/insert\n", incr_elapsed / PROF_INCREMENTAL_COUNT);
  printf("    Throughput: %.0f inserts/sec\n",
         (double)PROF_INCREMENTAL_COUNT / (incr_elapsed / 1000.0));

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, insert_errors,
                                "Similar vector inserts had errors");

  /* Verify search works — query with one of the similar vectors */
  printf("\n  Verifying search finds similar vectors...\n");

  DiskAnnResult results[PROF_VERIFY_K];
  float *query = similar_vectors; /* First similar vector */
  int n = diskann_search(idx, query, PROF_DIMS, PROF_VERIFY_K, results);
  TEST_ASSERT_TRUE_MESSAGE(n > 0, "search returned 0 results");

  /* At least some results should be from the similar batch (IDs > 10000) */
  int similar_hits = 0;
  for (int i = 0; i < n; i++) {
    if (results[i].id > PROF_BASE_SIZE) {
      similar_hits++;
    }
  }

  printf("    Found %d/%d results from similar batch (IDs > %d)\n",
         similar_hits, n, PROF_BASE_SIZE);

  /* Similar vectors are very close together, so most results should be
   * from the similar batch */
  TEST_ASSERT_TRUE_MESSAGE(
      similar_hits >= 1,
      "Expected at least 1 similar vector in search results");

  printf("\n=================================================================\n");

  /* Cleanup */
  diskann_close_index(idx);
  free(base_vectors);
  free(similar_vectors);
  sqlite3_close(db);
  cleanup_temp_files();
}

/**************************************************************************
** Main
**************************************************************************/

void setUp(void) {}
void tearDown(void) {}

int main(void) {
  atexit(cleanup_temp_files);

  UNITY_BEGIN();

  printf("\n");
  printf("====================================================\n");
  printf("DiskANN Insert Profiling Tests\n");
  printf("====================================================\n");
  printf("These tests profile incremental insert performance\n");
  printf("at scale and may take several minutes to complete.\n");
  printf("\n");
  printf("For detailed per-phase timing, run with:\n");
  printf("  DISKANN_DEBUG_TIMING=1 ./build/test_profiling 2>timing.csv\n");
  printf("\n");

  RUN_TEST(test_profile_incremental_10k);
  RUN_TEST(test_profile_similar_vectors);

  return UNITY_END();
}
