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
** Create a heap-allocated BlobSpot for testing.
** Uses sqlite3_malloc + memset(0) + refcount=1, matching blob_spot_create.
** Caller takes one reference (refcount=1). Must be balanced by blob_spot_free.
*/
static BlobSpot *create_mock_blobspot(void) {
  BlobSpot *spot = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
  if (spot) {
    memset(spot, 0, sizeof(BlobSpot));
    spot->refcount = 1;
  }
  return spot;
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

  BlobSpot *spot = create_mock_blobspot();
  TEST_ASSERT_NOT_NULL(spot);

  /* Put rowid=1 into cache (addrefs: 1→2) */
  blob_cache_put(&cache, 1, spot);
  TEST_ASSERT_EQUAL(1, cache.count);

  /* Get rowid=1 — should hit (addrefs: 2→3) */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);
  TEST_ASSERT_EQUAL_PTR(spot, result);
  TEST_ASSERT_EQUAL(1, cache.hits);
  TEST_ASSERT_EQUAL(0, cache.misses);

  blob_spot_free(result);    /* Release get ref (3→2) */
  blob_cache_deinit(&cache); /* Release cache ref (2→1) */
  blob_spot_free(spot);      /* Release creator ref (1→0→freed) */
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

  /* Create 15 heap-allocated BlobSpots */
  BlobSpot *blobs[15];
  for (int i = 0; i < 15; i++) {
    blobs[i] = create_mock_blobspot();
    TEST_ASSERT_NOT_NULL(blobs[i]);
  }

  /* Put 15 entries (capacity is 10, so first 5 evicted).
  ** Each put: addrefs (1→2). Eviction: decrefs (2→1). */
  for (int i = 0; i < 15; i++) {
    blob_cache_put(&cache, (uint64_t)(i + 1), blobs[i]);
  }

  TEST_ASSERT_EQUAL(10, cache.count); /* Capped at capacity */

  /* First 5 entries (rowids 1-5) should be evicted */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NULL(result); /* Evicted — no addref for miss */

  result = blob_cache_get(&cache, 5);
  TEST_ASSERT_NULL(result); /* Evicted */

  /* Last 10 entries (rowids 6-15) should still be cached */
  result = blob_cache_get(&cache, 6);
  TEST_ASSERT_NOT_NULL(result); /* Still cached */
  TEST_ASSERT_EQUAL_PTR(blobs[5], result);
  blob_spot_free(result); /* Release get ref */

  result = blob_cache_get(&cache, 15);
  TEST_ASSERT_NOT_NULL(result); /* Still cached */
  TEST_ASSERT_EQUAL_PTR(blobs[14], result);
  blob_spot_free(result); /* Release get ref */

  blob_cache_deinit(&cache); /* Release cache refs for 6-15 */

  /* Release all creator refs */
  for (int i = 0; i < 15; i++) {
    blob_spot_free(blobs[i]);
  }
}

/**************************************************************************
** Test 5: LRU eviction — get promotes to head
**************************************************************************/

void test_cache_hit_promotes(void) {
  BlobCache cache;
  blob_cache_init(&cache, 3); /* Tiny capacity */

  BlobSpot *blobs[4];
  for (int i = 0; i < 4; i++) {
    blobs[i] = create_mock_blobspot();
    TEST_ASSERT_NOT_NULL(blobs[i]);
  }

  /* Put rowids 1, 2, 3 (fills cache) */
  blob_cache_put(&cache, 1, blobs[0]);
  blob_cache_put(&cache, 2, blobs[1]);
  blob_cache_put(&cache, 3, blobs[2]);

  /* Get rowid=1 (should promote to head) */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);
  blob_spot_free(result); /* Release get ref */

  /* Put rowid=4 (should evict 2, not 1, since 1 was promoted) */
  blob_cache_put(&cache, 4, blobs[3]);

  /* Verify rowid=1 still cached */
  result = blob_cache_get(&cache, 1);
  TEST_ASSERT_NOT_NULL(result);
  blob_spot_free(result); /* Release get ref */

  /* Verify rowid=2 was evicted */
  result = blob_cache_get(&cache, 2);
  TEST_ASSERT_NULL(result);

  blob_cache_deinit(&cache); /* Release cache refs */

  /* Release all creator refs */
  for (int i = 0; i < 4; i++) {
    blob_spot_free(blobs[i]);
  }
}

/**************************************************************************
** Test 6: Stats counters — hits and misses
**************************************************************************/

void test_cache_stats(void) {
  BlobCache cache;
  blob_cache_init(&cache, 100);

  BlobSpot *blobs[5];
  for (int i = 0; i < 5; i++) {
    blobs[i] = create_mock_blobspot();
    TEST_ASSERT_NOT_NULL(blobs[i]);
    blob_cache_put(&cache, (uint64_t)(i + 1), blobs[i]);
  }

  /* 3 hits — each get addrefs, must decref */
  BlobSpot *r;
  r = blob_cache_get(&cache, 1);
  blob_spot_free(r);
  r = blob_cache_get(&cache, 2);
  blob_spot_free(r);
  r = blob_cache_get(&cache, 3);
  blob_spot_free(r);

  /* 2 misses — return NULL, no ref to manage */
  blob_cache_get(&cache, 999);
  blob_cache_get(&cache, 1000);

  TEST_ASSERT_EQUAL(3, cache.hits);
  TEST_ASSERT_EQUAL(2, cache.misses);

  blob_cache_deinit(&cache);

  /* Release creator refs */
  for (int i = 0; i < 5; i++) {
    blob_spot_free(blobs[i]);
  }
}

/**************************************************************************
** Test 7: NULL safety — defensive programming
**************************************************************************/

void test_cache_null_safety(void) {
  BlobSpot *spot = create_mock_blobspot();
  TEST_ASSERT_NOT_NULL(spot);

  /* blob_cache_get with NULL cache — should not crash */
  BlobSpot *result = blob_cache_get(NULL, 1);
  TEST_ASSERT_NULL(result);

  /* blob_cache_put with NULL cache — should not crash (no addref) */
  blob_cache_put(NULL, 1, spot);
  TEST_ASSERT_EQUAL(1, spot->refcount); /* Unchanged */

  /* blob_cache_deinit with NULL — should not crash */
  blob_cache_deinit(NULL);

  /* If we get here without segfault, test passes */
  blob_spot_free(spot);
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

  BlobSpot *spot1 = create_mock_blobspot();
  BlobSpot *spot2 = create_mock_blobspot();
  TEST_ASSERT_NOT_NULL(spot1);
  TEST_ASSERT_NOT_NULL(spot2);

  /* Put rowid=1 first time (spot1: 1→2) */
  blob_cache_put(&cache, 1, spot1);
  TEST_ASSERT_EQUAL(1, cache.count);

  /* Put rowid=1 again — replaces spot1 with spot2.
  ** spot1: cache releases old ref (2→1). spot2: cache addrefs (1→2). */
  blob_cache_put(&cache, 1, spot2);
  TEST_ASSERT_EQUAL(1, cache.count);     /* Count shouldn't increase */
  TEST_ASSERT_EQUAL(1, spot1->refcount); /* Old ref released */
  TEST_ASSERT_EQUAL(2, spot2->refcount); /* New ref taken */

  /* Get should return second pointer (addrefs: 2→3) */
  BlobSpot *result = blob_cache_get(&cache, 1);
  TEST_ASSERT_EQUAL_PTR(spot2, result);
  blob_spot_free(result); /* Release get ref (3→2) */

  blob_cache_deinit(&cache); /* Release cache ref on spot2 (2→1) */

  blob_spot_free(spot1); /* 1→0→freed */
  blob_spot_free(spot2); /* 1→0→freed */
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

/**************************************************************************
** Test 11: Cache put increments refcount
**************************************************************************/

void test_cache_owning_put_sets_flag(void) {
  BlobCache cache;
  blob_cache_init(&cache, 10);

  /* Allocate a mock BlobSpot with refcount=1 (simulates blob_spot_create) */
  BlobSpot *spot = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
  TEST_ASSERT_NOT_NULL(spot);
  memset(spot, 0, sizeof(BlobSpot));
  spot->refcount = 1; /* Creator's reference */
  TEST_ASSERT_EQUAL(1, spot->refcount);

  /* Put into cache — should addref (refcount 1 -> 2) */
  blob_cache_put(&cache, 1, spot);
  TEST_ASSERT_EQUAL(2, spot->refcount);

  /* Deinit releases cache's ref (refcount 2 -> 1) */
  blob_cache_deinit(&cache);
  TEST_ASSERT_EQUAL(1, spot->refcount);

  /* Release creator's ref (refcount 1 -> 0 -> freed) */
  blob_spot_free(spot);
}

/**************************************************************************
** Test 12: Cache eviction decrements refcount
**************************************************************************/

void test_cache_owning_eviction_clears_flag(void) {
  BlobCache cache;
  blob_cache_init(&cache, 3); /* Tiny capacity */

  /* Allocate 4 BlobSpots with refcount=1 */
  BlobSpot *spots[4];
  for (int i = 0; i < 4; i++) {
    spots[i] = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
    TEST_ASSERT_NOT_NULL(spots[i]);
    memset(spots[i], 0, sizeof(BlobSpot));
    spots[i]->refcount = 1;
  }

  /* Fill cache (capacity 3) — each put addrefs to 2 */
  blob_cache_put(&cache, 1, spots[0]);
  blob_cache_put(&cache, 2, spots[1]);
  blob_cache_put(&cache, 3, spots[2]);
  TEST_ASSERT_EQUAL(2, spots[0]->refcount);
  TEST_ASSERT_EQUAL(2, spots[1]->refcount);
  TEST_ASSERT_EQUAL(2, spots[2]->refcount);

  /* Put 4th entry — evicts LRU (rowid=1, spots[0]).
  ** Eviction decrefs spots[0] from 2 to 1 (creator still holds ref). */
  blob_cache_put(&cache, 4, spots[3]);
  TEST_ASSERT_EQUAL(1, spots[0]->refcount); /* Evicted, creator ref remains */
  TEST_ASSERT_EQUAL(2, spots[3]->refcount); /* Cached */

  /* Deinit releases cache refs for spots[1-3] */
  blob_cache_deinit(&cache);

  /* Release creator refs */
  blob_spot_free(spots[0]); /* refcount 1 -> 0 -> freed */
  blob_spot_free(spots[1]); /* refcount 1 -> 0 -> freed */
  blob_spot_free(spots[2]); /* refcount 1 -> 0 -> freed */
  blob_spot_free(spots[3]); /* refcount 1 -> 0 -> freed */
}

/**************************************************************************
** Test 13: Cache deinit releases all refs
**************************************************************************/

void test_cache_owning_deinit_frees(void) {
  BlobCache cache;
  blob_cache_init(&cache, 10);

  /* Allocate and insert 5 BlobSpots (refcount=1 each) */
  BlobSpot *spots[5];
  for (int i = 0; i < 5; i++) {
    spots[i] = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
    TEST_ASSERT_NOT_NULL(spots[i]);
    memset(spots[i], 0, sizeof(BlobSpot));
    spots[i]->refcount = 1;
    blob_cache_put(&cache, (uint64_t)(i + 1), spots[i]);
    TEST_ASSERT_EQUAL(2, spots[i]->refcount);
  }

  TEST_ASSERT_EQUAL(5, cache.count);

  /* Deinit releases cache refs (refcount 2 -> 1 each) */
  blob_cache_deinit(&cache);

  /* Release creator refs (refcount 1 -> 0 -> freed) */
  for (int i = 0; i < 5; i++) {
    blob_spot_free(spots[i]);
  }
}

/**************************************************************************
** Test 14: Cache get increments refcount
**************************************************************************/

void test_cache_non_owning_no_flag(void) {
  BlobCache cache;
  blob_cache_init(&cache, 10);

  BlobSpot *spot = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
  TEST_ASSERT_NOT_NULL(spot);
  memset(spot, 0, sizeof(BlobSpot));
  spot->refcount = 1;

  blob_cache_put(&cache, 1, spot);
  TEST_ASSERT_EQUAL(2, spot->refcount); /* Creator + cache */

  /* Get addrefs (refcount 2 -> 3) */
  BlobSpot *retrieved = blob_cache_get(&cache, 1);
  TEST_ASSERT_EQUAL_PTR(spot, retrieved);
  TEST_ASSERT_EQUAL(3, spot->refcount); /* Creator + cache + get */

  /* Release get ref */
  blob_spot_free(retrieved); /* 3 -> 2 */

  blob_cache_deinit(&cache); /* 2 -> 1 */
  blob_spot_free(spot);      /* 1 -> 0 -> freed */
}

/**************************************************************************
** Test 15: Same-pointer re-insert doesn't leak refcount
**************************************************************************/

void test_cache_same_pointer_no_leak(void) {
  BlobCache cache;
  blob_cache_init(&cache, 10);

  BlobSpot *spot = create_mock_blobspot();
  TEST_ASSERT_NOT_NULL(spot);

  /* Put rowid=1 (addrefs: 1→2) */
  blob_cache_put(&cache, 1, spot);
  TEST_ASSERT_EQUAL(2, spot->refcount);

  /* Put same pointer for same rowid again — should NOT change refcount */
  blob_cache_put(&cache, 1, spot);
  TEST_ASSERT_EQUAL(2, spot->refcount); /* Still 2, not 3 */

  blob_cache_deinit(&cache); /* 2 → 1 */
  blob_spot_free(spot);      /* 1 → 0 → freed */
}
