/*
** Tests for BLOB cache (build speed optimization)
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** Test strategy:
**
** 1. LIFECYCLE TESTS — init/deinit
**    - Verify cache structure is properly initialized
**    - Verify deinit frees resources without leaks
**
** 2. BASIC OPERATIONS — put/get
**    - Cache hit returns same BlobSpot pointer
**    - Cache miss returns NULL
**    - Stats counters increment correctly
**
** 3. LRU EVICTION — least recently used eviction
**    - When capacity reached, oldest entry evicted
**    - Recently used entries promoted to head
**    - Eviction order follows LRU semantics
**
** 4. NULL SAFETY — defensive programming
**    - All functions handle NULL pointers gracefully
**
** Note: These tests will FAIL to compile initially because diskann_cache.h
** doesn't exist yet. This is correct TDD — tests define the API contract
** before implementation.
*/
#include "unity/unity.h"
#include <stdlib.h>
#include <string.h>

/* Implementation now exists */
#include "../../src/diskann_blob.h"
#include "../../src/diskann_cache.h"

/**************************************************************************
** Test helpers
**************************************************************************/

/*
** Mock BlobSpot structure for testing.
** Cache doesn't own BlobSpots, just holds pointers, so we can use
** fake structures (just need unique addresses for pointer equality tests).
*/
typedef struct {
  uint64_t rowid;
  int dummy_data;
} MockBlobSpot;

/* Create mock BlobSpot with given rowid (on stack, not heap) */
static void init_mock_blob(MockBlobSpot *blob, uint64_t rowid) {
  blob->rowid = rowid;
  blob->dummy_data = (int)rowid;
}

/**************************************************************************
** Test 1: Lifecycle — init/deinit
**************************************************************************/

void test_cache_init_deinit(void) {
  BlobCache cache;
  int rc = blob_cache_init(&cache, 100);

  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(100, cache.capacity);
  TEST_ASSERT_EQUAL(0, cache.count);
  TEST_ASSERT_NOT_NULL(cache.slots);
  TEST_ASSERT_NOT_NULL(cache.rowids);
  TEST_ASSERT_EQUAL(0, cache.hits);
  TEST_ASSERT_EQUAL(0, cache.misses);

  blob_cache_deinit(&cache);
  /* Valgrind will verify no leaks */
}

/**************************************************************************
** Test 2: Basic operations — put/get hit
**************************************************************************/

void test_cache_put_get_hit(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  /* Create mock BlobSpot */
  MockBlobSpot mock_blob;
  init_mock_blob(&mock_blob, 1);

  /* Put rowid=1 into cache */
  blob_cache_put(&cache, 1, (BlobSpot *)&mock_blob);
  TEST_ASSERT_EQUAL(1, cache.count);

  /* Get rowid=1 — should hit */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);
  TEST_ASSERT_EQUAL_PTR(&mock_blob, result);
  TEST_ASSERT_EQUAL(1, cache.hits);
  TEST_ASSERT_EQUAL(0, cache.misses);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 3: Basic operations — get miss
**************************************************************************/

void test_cache_put_get_miss(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  /* Get rowid=999 (not in cache) — should miss */
  BlobSpot *result = blob_cache_get(&cache, 999);
  TEST_ASSERT_NULL(result);
  TEST_ASSERT_EQUAL(0, cache.hits);
  TEST_ASSERT_EQUAL(1, cache.misses);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 4: LRU eviction — evict oldest when full
**************************************************************************/

void test_cache_eviction_lru(void) {
  BlobCache cache;
  blob_cache_init(&cache, 10); /* Small capacity for testing */

  /* Create 15 mock BlobSpots */
  MockBlobSpot blobs[15];
  for (int i = 0; i < 15; i++) {
    init_mock_blob(&blobs[i], (uint64_t)(i + 1));
  }

  /* Put 15 entries (capacity is 10, so 5 should be evicted) */
  for (int i = 0; i < 15; i++) {
    blob_cache_put(&cache, (uint64_t)(i + 1), (BlobSpot *)&blobs[i]);
  }

  TEST_ASSERT_EQUAL(10, cache.count); /* Capped at capacity */

  /* First 5 entries (rowids 1-5) should be evicted */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NULL(result); /* Evicted */

  result = blob_cache_get(&cache, 5);
  TEST_ASSERT_NULL(result); /* Evicted */

  /* Last 10 entries (rowids 6-15) should still be cached */
  result = blob_cache_get(&cache, 6);
  TEST_ASSERT_NOT_NULL(result); /* Still cached */
  TEST_ASSERT_EQUAL_PTR(&blobs[5], result);

  result = blob_cache_get(&cache, 15);
  TEST_ASSERT_NOT_NULL(result); /* Still cached */
  TEST_ASSERT_EQUAL_PTR(&blobs[14], result);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 5: LRU eviction — get promotes to head
**************************************************************************/

void test_cache_hit_promotes(void) {
  BlobCache cache;
  blob_cache_init(&cache, 3); /* Tiny capacity */

  MockBlobSpot blobs[4];
  for (int i = 0; i < 4; i++) {
    init_mock_blob(&blobs[i], (uint64_t)(i + 1));
  }

  /* Put rowids 1, 2, 3 (fills cache) */
  blob_cache_put(&cache, 1, (BlobSpot *)&blobs[0]);
  blob_cache_put(&cache, 2, (BlobSpot *)&blobs[1]);
  blob_cache_put(&cache, 3, (BlobSpot *)&blobs[2]);

  /* Get rowid=1 (should promote to head) */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);

  /* Put rowid=4 (should evict 2, not 1, since 1 was promoted) */
  blob_cache_put(&cache, 4, (BlobSpot *)&blobs[3]);

  /* Verify rowid=1 still cached */
  result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);

  /* Verify rowid=2 was evicted */
  result = blob_cache_get(&cache, 2);
  TEST_ASSERT_NULL(result);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 6: Stats counters — hits and misses
**************************************************************************/

void test_cache_stats(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  MockBlobSpot blobs[5];
  for (int i = 0; i < 5; i++) {
    init_mock_blob(&blobs[i], (uint64_t)(i + 1));
    blob_cache_put(&cache, (uint64_t)(i + 1), (BlobSpot *)&blobs[i]);
  }

  /* 3 hits */
  blob_cache_get(&cache, 1); /* hit */
  blob_cache_get(&cache, 2); /* hit */
  blob_cache_get(&cache, 3); /* hit */

  /* 2 misses */
  blob_cache_get(&cache, 999);  /* miss */
  blob_cache_get(&cache, 1000); /* miss */

  TEST_ASSERT_EQUAL(3, cache.hits);
  TEST_ASSERT_EQUAL(2, cache.misses);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 7: NULL safety — defensive programming
**************************************************************************/

void test_cache_null_safety(void) {
  MockBlobSpot mock_blob;
  init_mock_blob(&mock_blob, 1);

  /* blob_cache_get with NULL cache — should not crash */
  BlobSpot *result = blob_cache_get(NULL, 1);
  TEST_ASSERT_NULL(result);

  /* blob_cache_put with NULL cache — should not crash */
  blob_cache_put(NULL, 1, (BlobSpot *)&mock_blob);

  /* blob_cache_deinit with NULL — should not crash */
  blob_cache_deinit(NULL);

  /* If we get here without segfault, test passes */
  TEST_ASSERT(1);
}

/**************************************************************************
** Test 8: Put NULL BlobSpot — should be handled gracefully
**************************************************************************/

void test_cache_put_null_blob(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  /* Put NULL BlobSpot — should be ignored or handled gracefully */
  blob_cache_put(&cache, 1, NULL);

  /* Get should return NULL */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NULL(result);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 9: Put duplicate rowid — should update existing entry
**************************************************************************/

void test_cache_put_duplicate(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  MockBlobSpot blob1, blob2;
  init_mock_blob(&blob1, 1);
  init_mock_blob(&blob2, 1);
  blob2.dummy_data = 999; /* Different data */

  /* Put rowid=1 first time */
  blob_cache_put(&cache, 1, (BlobSpot *)&blob1);
  TEST_ASSERT_EQUAL(1, cache.count);

  /* Put rowid=1 again with different pointer */
  blob_cache_put(&cache, 1, (BlobSpot *)&blob2);
  TEST_ASSERT_EQUAL(1, cache.count); /* Count shouldn't increase */

  /* Get should return second pointer */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_EQUAL_PTR(&blob2, result);

  blob_cache_deinit(&cache);
}

/**************************************************************************
** Test 10: Large capacity — verify no integer overflow
**************************************************************************/

void test_cache_large_capacity(void) {
  BlobCache cache;

  /* Test with large but reasonable capacity */
  int rc = blob_cache_init(&cache, 1000);
  TEST_ASSERT_EQUAL(DISKANN_OK, rc);
  TEST_ASSERT_EQUAL(1000, cache.capacity);

  blob_cache_deinit(&cache);
}
