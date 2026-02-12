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
** - Cache lifetime: single insert operation (no stale data across inserts)
** - Cache does NOT own BlobSpots (caller creates, caller frees)
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
** - BlobSpot instances: NOT owned by cache (caller manages)
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
** Note: Cache does NOT take ownership of BlobSpot. Caller must free.
*/
void blob_cache_put(BlobCache *cache, uint64_t rowid, BlobSpot *spot);

/*
** Free cache resources.
**
** Does NOT free BlobSpot instances (caller owns them).
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
