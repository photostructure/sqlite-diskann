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

  /* Batch mode: persistent cache across multiple inserts */
  BlobCache *batch_cache; /* NULL when not in batch mode */
};

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
