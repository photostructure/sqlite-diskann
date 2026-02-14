/*
** DiskANN BLOB Cache
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** LRU cache for BlobSpot instances to reduce repeated BLOB I/O during insert.
** Hot nodes (early nodes with low rowid) are read 100+ times during graph
** construction. Caching reduces 400GB → 160GB BLOB I/O on 25k vectors.
**
** Design:
** - Simple LRU eviction with doubly-linked list (array-based, not pointers)
** - Linear search for get (100 entries = ~10 cache lines, fast enough)
** - Ownership via BlobSpot refcount: cache takes a ref on put/get,
**   releases on eviction/deinit. BlobSpot freed when refcount reaches 0.
*/
#ifndef DISKANN_CACHE_H
#define DISKANN_CACHE_H

#include "diskann_blob.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** BlobCache - LRU cache for BlobSpot instances
**
** Memory ownership:
** - slots, rowids, next, prev: owned by cache (freed in deinit)
** - BlobSpot instances: managed via refcount. Cache takes a ref on
**   put/get, releases on eviction/deinit. BlobSpot freed when last
**   ref is released (refcount reaches 0).
**
** LRU implementation:
** - head: most recently used (MRU)
** - tail: least recently used (LRU)
** - Eviction: remove tail when cache is full
** - Promotion: move to head on cache hit
*/
typedef struct BlobCache {
  BlobSpot **slots; /* Array of BlobSpot pointers (size = capacity) */
  uint64_t *rowids; /* Parallel array of rowids (size = capacity) */
  int *next;        /* Next index in LRU chain (-1 = end) */
  int *prev;        /* Previous index in LRU chain (-1 = end) */
  int capacity;     /* Maximum entries */
  int count;        /* Current entries */
  int head;         /* Index of MRU entry (-1 if empty) */
  int tail;         /* Index of LRU entry (-1 if empty) */
  int hits;         /* Cache hit counter */
  int misses;       /* Cache miss counter */
} BlobCache;

/*
** Initialize cache with given capacity.
**
** Parameters:
**   cache    - Cache structure to initialize (must not be NULL)
**   capacity - Maximum number of entries (typically 100)
**
** Returns:
**   DISKANN_OK on success
**   DISKANN_ERROR_NOMEM if allocation fails
**
** Caller must call blob_cache_deinit() when done.
*/
int blob_cache_init(BlobCache *cache, int capacity);

/*
** Get BlobSpot for given rowid (NULL if not found).
**
** Increments hits counter on success, misses counter on failure.
** Promotes entry to MRU on hit.
**
** Parameters:
**   cache - Cache to search (NULL safe - returns NULL)
**   rowid - Row ID to look up
**
** Returns:
**   BlobSpot pointer if found, NULL otherwise
*/
BlobSpot *blob_cache_get(BlobCache *cache, uint64_t rowid);

/*
** Put BlobSpot into cache for given rowid.
**
** If cache is full, evicts LRU entry.
** If rowid already exists, updates the BlobSpot pointer.
** New/updated entry is promoted to MRU.
**
** Parameters:
**   cache - Cache to update (NULL safe - no-op)
**   rowid - Row ID to associate with BlobSpot
**   spot  - BlobSpot to cache (can be NULL - stores NULL pointer)
**
** Note: Cache takes a refcount reference. Caller retains its own reference.
*/
void blob_cache_put(BlobCache *cache, uint64_t rowid, BlobSpot *spot);

/*
** Close all blob handles in the cache, preserving buffer data.
**
** Used by vtab xUpdate to release blob handles so they don't block
** COMMIT. Cached BlobSpots are marked as aborted; blob_spot_reload()
** will reopen handles on next access.
**
** Parameters:
**   cache - Cache to release handles for (NULL safe - no-op)
*/
void blob_cache_release_handles(BlobCache *cache);

/*
** Free cache resources.
**
** Releases the cache's refcount reference on each BlobSpot.
** BlobSpots with no remaining references are freed.
** Safe to call with NULL.
**
** Parameters:
**   cache - Cache to deinitialize (can be NULL)
*/
void blob_cache_deinit(BlobCache *cache);

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_CACHE_H */
