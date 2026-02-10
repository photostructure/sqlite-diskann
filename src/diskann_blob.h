/*
** DiskANN BLOB I/O Layer
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
** MIT License
**
** This module provides utilities for reading/writing graph nodes as BLOBs
** in the shadow table using SQLite's incremental BLOB I/O API.
*/
#ifndef DISKANN_BLOB_H
#define DISKANN_BLOB_H

#include "diskann_internal.h"
#include <sqlite3.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** BlobSpot - Handle for incremental BLOB I/O
**
** Manages a buffer and SQLite BLOB handle for reading/writing graph nodes.
** Supports reusing the same handle for different rowids (optimization).
**
** Memory ownership:
** - pBlob: owned by this struct (closed in blob_spot_free)
** - pBuffer: owned by this struct (freed in blob_spot_free)
** - All other fields: simple values
*/
typedef struct BlobSpot {
  sqlite3_blob *pBlob;      /* SQLite BLOB handle */
  uint64_t rowid;           /* Current rowid */
  uint8_t *buffer;          /* Data buffer (typically 4KB) */
  uint32_t buffer_size;     /* Buffer size in bytes */
  int is_writable;          /* 1 if opened for writing, 0 for reading */
  int is_initialized;       /* 1 if buffer contains valid data */
  int is_aborted;           /* 1 if BLOB operations have been aborted */
} BlobSpot;

/*
** BLOB access modes (values passed to blob_spot_create is_writable parameter)
*/
#define DISKANN_BLOB_READONLY  0
#define DISKANN_BLOB_WRITABLE  1

/*
** Error codes specific to BLOB operations
*/
#define DISKANN_ROW_NOT_FOUND -100  /* Row doesn't exist in shadow table */

/*
** Create a new BlobSpot for the specified rowid.
**
** Opens a BLOB handle to the shadow table and allocates a buffer.
** The BLOB is opened for reading or writing based on is_writable.
**
** Parameters:
**   idx          - Index handle
**   out          - Pointer to receive BlobSpot (must not be NULL)
**   rowid        - Row ID in shadow table
**   buffer_size  - Buffer size (typically index->block_size)
**   is_writable  - 1 for write access, 0 for read-only
**
** Returns:
**   DISKANN_OK on success
**   DISKANN_ERROR_NOMEM if allocation fails
**   DISKANN_ROW_NOT_FOUND if rowid doesn't exist
**   DISKANN_ERROR on other SQLite errors
**
** Caller takes ownership of the BlobSpot and must call blob_spot_free().
*/
int blob_spot_create(
  DiskAnnIndex *idx,
  BlobSpot **out,
  uint64_t rowid,
  uint32_t buffer_size,
  int is_writable
);

/*
** Reload BlobSpot for a different rowid (or refresh current rowid).
**
** Reuses the existing BLOB handle if possible (via sqlite3_blob_reopen).
** If the rowid matches and data is already initialized, does nothing.
**
** Parameters:
**   idx          - Index handle
**   spot         - BlobSpot to reload (must not be NULL)
**   rowid        - Row ID to load
**   buffer_size  - Expected buffer size (must match spot->buffer_size)
**
** Returns:
**   DISKANN_OK on success
**   DISKANN_ROW_NOT_FOUND if rowid doesn't exist
**   DISKANN_ERROR on other errors
**
** After successful reload, spot->is_initialized is set to 1.
*/
int blob_spot_reload(
  DiskAnnIndex *idx,
  BlobSpot *spot,
  uint64_t rowid,
  uint32_t buffer_size
);

/*
** Flush BlobSpot buffer to database.
**
** Writes the buffer contents back to the BLOB in the shadow table.
** Only works if BlobSpot was opened with is_writable=1.
**
** Parameters:
**   idx   - Index handle
**   spot  - BlobSpot to flush (must not be NULL)
**
** Returns:
**   DISKANN_OK on success
**   DISKANN_ERROR on failure
*/
int blob_spot_flush(DiskAnnIndex *idx, BlobSpot *spot);

/*
** Free BlobSpot and associated resources.
**
** Closes the BLOB handle and frees the buffer.
** Safe to call with NULL.
**
** Parameters:
**   spot - BlobSpot to free (can be NULL)
*/
void blob_spot_free(BlobSpot *spot);

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_BLOB_H */
