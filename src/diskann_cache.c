/*
** DiskANN BLOB Cache Implementation
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "diskann_cache.h"
#include "diskann_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*
** Initialize cache with given capacity.
*/
int blob_cache_init(BlobCache *cache, int capacity) {
  if (!cache || capacity <= 0) {
    return DISKANN_ERROR;
  }

  memset(cache, 0, sizeof(BlobCache));
  cache->capacity = capacity;

  /* Allocate parallel arrays */
  cache->slots =
      (BlobSpot **)sqlite3_malloc64((uint64_t)capacity * sizeof(BlobSpot *));
  cache->rowids =
      (uint64_t *)sqlite3_malloc64((uint64_t)capacity * sizeof(uint64_t));
  cache->next = (int *)sqlite3_malloc64((uint64_t)capacity * sizeof(int));
  cache->prev = (int *)sqlite3_malloc64((uint64_t)capacity * sizeof(int));

  if (!cache->slots || !cache->rowids || !cache->next || !cache->prev) {
    blob_cache_deinit(cache);
    return DISKANN_ERROR_NOMEM;
  }

  /* Initialize arrays */
  memset(cache->slots, 0, (size_t)capacity * sizeof(BlobSpot *));
  memset(cache->rowids, 0, (size_t)capacity * sizeof(uint64_t));
  for (int i = 0; i < capacity; i++) {
    cache->next[i] = -1;
    cache->prev[i] = -1;
  }

  cache->head = -1;
  cache->tail = -1;
  cache->count = 0;
  cache->hits = 0;
  cache->misses = 0;

  return DISKANN_OK;
}

/*
** Find index of entry with given rowid.
** Returns index if found, -1 otherwise.
*/
static int find_entry(const BlobCache *cache, uint64_t rowid) {
  assert(cache);

  /* Linear search (fast for ~100 entries, ~10 cache lines) */
  for (int idx = cache->head; idx != -1; idx = cache->next[idx]) {
    if (cache->rowids[idx] == rowid) {
      return idx;
    }
  }
  return -1;
}

/*
** Remove entry at given index from LRU chain (does not free memory).
*/
static void remove_from_chain(BlobCache *cache, int idx) {
  assert(cache && idx >= 0 && idx < cache->capacity);

  int p = cache->prev[idx];
  int n = cache->next[idx];

  /* Update previous node's next pointer */
  if (p != -1) {
    cache->next[p] = n;
  } else {
    cache->head = n; /* idx was head */
  }

  /* Update next node's prev pointer */
  if (n != -1) {
    cache->prev[n] = p;
  } else {
    cache->tail = p; /* idx was tail */
  }

  cache->prev[idx] = -1;
  cache->next[idx] = -1;
}

/*
** Insert entry at head of LRU chain (most recently used).
*/
static void insert_at_head(BlobCache *cache, int idx) {
  assert(cache && idx >= 0 && idx < cache->capacity);

  cache->next[idx] = cache->head;
  cache->prev[idx] = -1;

  if (cache->head != -1) {
    cache->prev[cache->head] = idx;
  }

  cache->head = idx;

  if (cache->tail == -1) {
    cache->tail = idx; /* First entry */
  }
}

/*
** Promote entry to head (most recently used).
*/
static void promote_to_head(BlobCache *cache, int idx) {
  assert(cache && idx >= 0 && idx < cache->capacity);

  if (idx == cache->head) {
    return; /* Already at head */
  }

  remove_from_chain(cache, idx);
  insert_at_head(cache, idx);
}

/*
** Get BlobSpot for given rowid.
*/
BlobSpot *blob_cache_get(BlobCache *cache, uint64_t rowid) {
  if (!cache) {
    return NULL;
  }

  int idx = find_entry(cache, rowid);

  if (idx == -1) {
    cache->misses++;
    return NULL;
  }

  cache->hits++;
  promote_to_head(cache, idx);
  blob_spot_addref(cache->slots[idx]); /* Caller gets a reference */
  return cache->slots[idx];
}

/*
** Find free slot index or allocate one by evicting LRU entry.
** Returns index to use, always valid.
**
** Releases the cache's refcount reference on eviction. The BlobSpot is
** freed only if no other references remain (e.g., no DiskAnnNode in an
** active search context holds a reference).
*/
static int get_free_slot(BlobCache *cache) {
  assert(cache);

  if (cache->count < cache->capacity) {
    /* Use next available slot */
    return cache->count;
  }

  /* Cache is full - evict tail (LRU) */
  assert(cache->tail != -1);
  int idx = cache->tail;

  /* Release cache's reference — frees BlobSpot only if last ref */
  if (cache->slots[idx] != NULL) {
    blob_spot_free(cache->slots[idx]);
    cache->slots[idx] = NULL;
  }

  remove_from_chain(cache, idx);
  cache->count--; /* Will be re-incremented in put */
  return idx;
}

/*
** Put BlobSpot into cache.
*/
void blob_cache_put(BlobCache *cache, uint64_t rowid, BlobSpot *spot) {
  if (!cache) {
    return;
  }

  /* Check if rowid already exists */
  int idx = find_entry(cache, rowid);

  if (idx != -1) {
    /* Update existing entry */
    if (cache->slots[idx] == spot) {
      /* Same pointer — no ref change needed, just promote */
      promote_to_head(cache, idx);
      return;
    }
    if (cache->slots[idx] != NULL) {
      blob_spot_free(cache->slots[idx]); /* Release old ref */
    }
    cache->slots[idx] = spot;
    if (spot) {
      blob_spot_addref(spot); /* Cache takes a reference */
    }
    promote_to_head(cache, idx);
    return;
  }

  /* Allocate new slot (may evict LRU, releasing its ref) */
  idx = get_free_slot(cache);
  assert(idx >= 0 && idx < cache->capacity);

  cache->slots[idx] = spot;
  cache->rowids[idx] = rowid;
  cache->count++;

  /* Cache takes a reference */
  if (spot) {
    blob_spot_addref(spot);
  }

  insert_at_head(cache, idx);
}

/*
** Close all blob handles in the cache, preserving buffer data.
** Marks each BlobSpot as aborted so blob_spot_reload() will reopen.
*/
void blob_cache_release_handles(BlobCache *cache) {
  if (!cache || !cache->slots) {
    return;
  }

  for (int idx = cache->head; idx != -1; idx = cache->next[idx]) {
    BlobSpot *spot = cache->slots[idx];
    if (spot && spot->pBlob) {
      sqlite3_blob_close(spot->pBlob);
      spot->pBlob = NULL;
      spot->is_aborted = 1;
    }
  }
}

/*
** Free cache resources.
**
** Releases the cache's refcount reference on each BlobSpot. BlobSpots
** with no remaining references are freed automatically.
*/
void blob_cache_deinit(BlobCache *cache) {
  if (!cache) {
    return;
  }

  /* Release cache's reference on all BlobSpots */
  if (cache->slots) {
    for (int idx = cache->head; idx != -1;) {
      int next = cache->next[idx];
      if (cache->slots[idx] != NULL) {
        blob_spot_free(cache->slots[idx]);
        cache->slots[idx] = NULL;
      }
      idx = next;
    }
  }

  /* Free arrays */
  if (cache->slots) {
    sqlite3_free(cache->slots);
    cache->slots = NULL;
  }
  if (cache->rowids) {
    sqlite3_free(cache->rowids);
    cache->rowids = NULL;
  }
  if (cache->next) {
    sqlite3_free(cache->next);
    cache->next = NULL;
  }
  if (cache->prev) {
    sqlite3_free(cache->prev);
    cache->prev = NULL;
  }

  memset(cache, 0, sizeof(BlobCache));
}
