/*
** DiskANN: Disk-based Approximate Nearest Neighbor search for SQLite
**
** Public API for standalone SQLite extension
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
** copies of the Software, and to permit persons to whom the Software is
** furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
** OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
** SOFTWARE.
*/
#ifndef DISKANN_H
#define DISKANN_H

#include <sqlite3.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Error codes
*/
#define DISKANN_OK 0
#define DISKANN_ERROR (-1)
#define DISKANN_ERROR_NOMEM (-2)
#define DISKANN_ERROR_NOTFOUND (-3)
#define DISKANN_ERROR_INVALID (-4)
#define DISKANN_ERROR_DIMENSION (-5)
#define DISKANN_ERROR_IO (-6)
#define DISKANN_ERROR_EXISTS (-7)

/*
** Distance metrics
*/
#define DISKANN_METRIC_EUCLIDEAN 0
#define DISKANN_METRIC_COSINE 1
#define DISKANN_METRIC_DOT 2

/*
** Opaque index handle
*/
typedef struct DiskAnnIndex DiskAnnIndex;

/*
** Index configuration
*/
typedef struct DiskAnnConfig {
  uint32_t dimensions;       /* vector dimensionality (e.g., 768 for CLIP) */
  uint8_t metric;            /* DISKANN_METRIC_* */
  uint32_t max_neighbors;    /* max edges per node (default: 32) */
  uint32_t search_list_size; /* search beam width (default: 100) */
  uint32_t insert_list_size; /* insert beam width (default: 200) */
  uint32_t block_size;       /* node block size in bytes (default: 4096) */
} DiskAnnConfig;

/*
** Search result
*/
typedef struct DiskAnnResult {
  int64_t id;
  float distance;
} DiskAnnResult;

/*
** Create a new DiskANN index with the specified configuration.
**
** Parameters:
**   db          - SQLite database handle
**   db_name     - Database name (e.g., "main")
**   index_name  - Index name (e.g., "clip_vectors")
**   config      - Index configuration (if NULL, uses defaults)
**
** Returns:
**   DISKANN_OK on success, error code on failure
*/
int diskann_create_index(sqlite3 *db, const char *db_name,
                         const char *index_name, const DiskAnnConfig *config);

/*
** Open an existing DiskANN index.
**
** Parameters:
**   db          - SQLite database handle
**   db_name     - Database name
**   index_name  - Index name
**   out_index   - Pointer to receive index handle (must not be NULL)
**
** Returns:
**   DISKANN_OK on success, error code on failure
**
** The caller takes ownership of the returned index and must call
** diskann_close_index() when done.
*/
int diskann_open_index(sqlite3 *db, const char *db_name, const char *index_name,
                       DiskAnnIndex **out_index);

/*
** Close an index and free associated resources.
**
** Parameters:
**   idx - Index handle (can be NULL)
*/
void diskann_close_index(DiskAnnIndex *idx);

/*
** Insert a vector into the index.
**
** Parameters:
**   idx     - Index handle
**   id      - Vector ID (user-provided, e.g., image ID)
**   vector  - Vector data (float32 array)
**   dims    - Vector dimensions (must match index configuration)
**
** Returns:
**   DISKANN_OK on success, error code on failure
*/
int diskann_insert(DiskAnnIndex *idx, int64_t id, const float *vector,
                   uint32_t dims);

/*
** Search for k-nearest neighbors.
**
** Parameters:
**   idx     - Index handle
**   query   - Query vector (float32 array)
**   dims    - Query dimensions (must match index configuration)
**   k       - Number of results to return
**   results - Result array (caller must allocate k elements)
**
** Returns:
**   Number of results found (may be < k if index has fewer vectors),
**   or negative error code on failure
*/
int diskann_search(DiskAnnIndex *idx, const float *query, uint32_t dims, int k,
                   DiskAnnResult *results);

/*
** Delete a vector from the index.
**
** Parameters:
**   idx - Index handle
**   id  - Vector ID to delete
**
** Returns:
**   DISKANN_OK on success, error code on failure
*/
int diskann_delete(DiskAnnIndex *idx, int64_t id);

/*
** Drop an index (delete all data).
**
** Parameters:
**   db         - SQLite database handle
**   db_name    - Database name
**   index_name - Index name
**
** Returns:
**   DISKANN_OK on success, error code on failure
*/
int diskann_drop_index(sqlite3 *db, const char *db_name,
                       const char *index_name);

/*
** Clear an index (delete all vectors but keep structure).
**
** Parameters:
**   db         - SQLite database handle
**   db_name    - Database name
**   index_name - Index name
**
** Returns:
**   DISKANN_OK on success, error code on failure
*/
int diskann_clear_index(sqlite3 *db, const char *db_name,
                        const char *index_name);

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_H */
