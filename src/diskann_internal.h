/*
** DiskANN Internal Structures
**
** Derived from libSQL DiskANN implementation
** Copyright 2024 the libSQL authors
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** This header defines internal structures used by the DiskANN implementation.
** These are NOT part of the public API and should not be used by external code.
*/
#ifndef DISKANN_INTERNAL_H
#define DISKANN_INTERNAL_H

#include "diskann.h"
#include "diskann_sqlite.h"
#include <stdint.h>

/* Forward declaration to avoid circular include:
** diskann_cache.h → diskann_blob.h → diskann_internal.h */
typedef struct BlobCache BlobCache;

#ifdef __cplusplus
extern "C" {
#endif

/*
** Internal DiskAnnIndex structure
**
** This structure holds all state for an open DiskANN index.
** Memory ownership:
** - db: borrowed reference (owned by caller)
** - db_name, index_name, shadow_name: owned by this struct (must free)
** - All other fields: owned by this struct
*/
struct DiskAnnIndex {
  sqlite3 *db;       /* Database connection (borrowed) */
  char *db_name;     /* Database schema name (e.g., "main") - malloc'd */
  char *index_name;  /* Index name - malloc'd */
  char *shadow_name; /* Shadow table name (e.g., "idx_shadow") - malloc'd */

  /* Index configuration (loaded from metadata) */
  uint32_t dimensions;       /* Vector dimensionality */
  uint8_t metric;            /* Distance metric (DISKANN_METRIC_*) */
  uint32_t max_neighbors;    /* Max edges per node */
  uint32_t search_list_size; /* Search beam width */
  uint32_t insert_list_size; /* Insert beam width */
  uint32_t block_size;       /* Node block size in bytes */
  double pruning_alpha;      /* Edge pruning threshold (default 1.2) */

  /* Derived layout fields (computed from config at open time) */
  uint32_t nNodeVectorSize; /* dims * sizeof(float) for float32 */
  uint32_t nEdgeVectorSize; /* Same as nNodeVectorSize for float32-only */

  /* Statistics (for debugging/profiling) */
  uint64_t num_reads;  /* Number of BLOB reads */
  uint64_t num_writes; /* Number of BLOB writes */

  /* Cached max rowid for dynamic search list scaling (updated on insert) */
  int64_t cached_max_rowid;

  /* Batch mode: persistent cache across multiple inserts */
  BlobCache *batch_cache;                  /* NULL when not in batch mode */
  struct DeferredEdgeList *deferred_edges; /* NULL when not in batch mode */
};

/*
** Deferred back-edge for lazy batch repair.
**
** During batch insert, Phase 2 (back-edges to visited neighbors) is deferred.
** Each DeferredEdge records a candidate edge to apply later in a single
** repair pass at diskann_end_batch().
**
** Memory ownership:
** - vector: OWNED copy (malloc'd), freed by truncate/deinit
*/
typedef struct DeferredEdge {
  int64_t target_rowid;   /* Existing node to add back-edge TO */
  int64_t inserted_rowid; /* Newly-inserted node (edge source) */
  float distance;         /* Precomputed dist(target, inserted) */
  float *vector;          /* OWNED copy of inserted node's vector */
} DeferredEdge;

/*
** Growable array of deferred edges with fixed capacity.
** Initialized in diskann_begin_batch(), consumed by
*diskann_batch_repair_edges(),
** freed in diskann_end_batch().
*/
typedef struct DeferredEdgeList {
  DeferredEdge *edges;  /* Array of deferred edges (malloc'd) */
  int count;            /* Current number of entries */
  int capacity;         /* Max entries (fixed at init) */
  uint32_t vector_size; /* Bytes per vector copy (= idx->nNodeVectorSize) */
} DeferredEdgeList;

/* Default capacity for deferred edge list.
** Supports ~960 inserts at 13% acceptance × 130 visited nodes. */
#define DEFERRED_EDGE_LIST_DEFAULT_CAPACITY 16384

/*
** Initialize a deferred edge list with given capacity.
** Returns DISKANN_OK on success, DISKANN_ERROR_NOMEM on allocation failure.
*/
int deferred_edge_list_init(DeferredEdgeList *list, int capacity,
                            uint32_t vector_size);

/*
** Add a deferred edge. Copies vector (vector_size bytes).
** Returns DISKANN_OK on success.
** Returns DISKANN_ERROR if list is at capacity (caller should spillover).
** Returns DISKANN_ERROR_NOMEM if vector copy allocation fails.
*/
int deferred_edge_list_add(DeferredEdgeList *list, int64_t target_rowid,
                           int64_t inserted_rowid, float distance,
                           const float *vector);

/*
** Truncate list to saved_count, freeing vectors of discarded entries.
** Used for rollback safety: save count before insert, truncate on failure.
*/
void deferred_edge_list_truncate(DeferredEdgeList *list, int saved_count);

/*
** Free all entries (including vector copies) and the list array.
** Safe to call on a zeroed list. Does NOT free the list struct itself.
*/
void deferred_edge_list_deinit(DeferredEdgeList *list);

/*
** Apply all deferred edges in a single repair pass.
** Sorts by target_rowid, groups, loads each target once via batch_cache,
** re-checks acceptance with replace_edge_idx(), applies + prunes, flushes once.
** Must be called while idx->batch_cache is still alive.
** Returns DISKANN_OK on success or error code on failure.
*/
int diskann_batch_repair_edges(DiskAnnIndex *idx, DeferredEdgeList *list);

/*
** Metadata table name format: {index_name}_metadata
** Schema:
**   key TEXT PRIMARY KEY,
**   value INTEGER NOT NULL
**
** Keys (all stored as portable SQLite integers):
**   "dimensions"         - vector dimensionality
**   "metric"             - DISKANN_METRIC_*
**   "max_neighbors"      - max edges per node
**   "search_list_size"   - search beam width
**   "insert_list_size"   - insert beam width
**   "block_size"         - node block size in bytes
*/

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_INTERNAL_H */
