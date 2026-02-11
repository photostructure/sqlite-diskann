/*
** DiskANN BLOB I/O Layer Implementation
**
** Derived from libSQL DiskANN implementation
** Copyright 2024 the libSQL authors
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "diskann_blob.h"
#include "diskann.h"
#include "diskann_internal.h"
#include "diskann_sqlite.h"
#include <assert.h>
#include <string.h>

/*
** Convert SQLite error codes to DiskANN error codes.
** Special case: distinguish "row not found" from generic errors.
*/
static int convert_sqlite_error(DiskAnnIndex *idx, int rc) {
  if (rc == SQLITE_ERROR) {
    const char *msg = sqlite3_errmsg(idx->db);
    if (msg && strncmp(msg, "no such rowid", 13) == 0) {
      return DISKANN_ROW_NOT_FOUND;
    }
  }
  if (rc == SQLITE_OK) {
    return DISKANN_OK;
  }
  if (rc == SQLITE_NOMEM) {
    return DISKANN_ERROR_NOMEM;
  }
  return DISKANN_ERROR;
}

int blob_spot_create(DiskAnnIndex *idx, BlobSpot **out, uint64_t rowid,
                     uint32_t buffer_size, int is_writable) {
  BlobSpot *spot = NULL;
  uint8_t *buffer = NULL;
  int rc = DISKANN_OK;

  /* Validate inputs */
  if (!idx || !out) {
    return DISKANN_ERROR_INVALID;
  }
  if (buffer_size == 0) {
    return DISKANN_ERROR_INVALID;
  }

  /* Initialize output */
  *out = NULL;

  /* Allocate BlobSpot structure */
  spot = (BlobSpot *)sqlite3_malloc(sizeof(BlobSpot));
  if (!spot) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }
  memset(spot, 0, sizeof(BlobSpot));

  /* Allocate buffer */
  buffer = (uint8_t *)sqlite3_malloc((int)buffer_size);
  if (!buffer) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }

  /* Open BLOB handle - do this last so we don't need to close on error */
  int sqlite_rc =
      sqlite3_blob_open(idx->db, idx->db_name, idx->shadow_name, "data",
                        (sqlite3_int64)rowid, is_writable, &spot->pBlob);

  rc = convert_sqlite_error(idx, sqlite_rc);
  if (rc != DISKANN_OK) {
    goto cleanup;
  }

  /* Initialize BlobSpot fields */
  spot->rowid = rowid;
  spot->buffer = buffer;
  spot->buffer_size = buffer_size;
  spot->is_writable = is_writable;
  spot->is_initialized = 0;
  spot->is_aborted = 0;

  /* Success - transfer ownership to caller */
  *out = spot;
  return DISKANN_OK;

cleanup:
  /* Error path - free allocated resources */
  if (buffer) {
    sqlite3_free(buffer);
  }
  if (spot) {
    sqlite3_free(spot);
  }
  return rc;
}

int blob_spot_reload(DiskAnnIndex *idx, BlobSpot *spot, uint64_t rowid,
                     uint32_t buffer_size) {
  int rc;

  /* Validate inputs */
  if (!idx || !spot) {
    return DISKANN_ERROR_INVALID;
  }
  assert(spot->pBlob != NULL || spot->is_aborted);

  /* Runtime check for buffer size mismatch (prevents buffer overflow) */
  if (spot->buffer_size != buffer_size) {
    return DISKANN_ERROR_INVALID;
  }

  /* If already loaded and same rowid, nothing to do */
  if (spot->rowid == rowid && spot->is_initialized) {
    return DISKANN_OK;
  }

  /* Handle aborted BLOB - need to close and reopen */
  if (spot->is_aborted) {
    if (spot->pBlob) {
      sqlite3_blob_close(spot->pBlob);
      spot->pBlob = NULL;
    }

    /* Reopen BLOB */
    int sqlite_rc = sqlite3_blob_open(idx->db, idx->db_name, idx->shadow_name,
                                      "data", (sqlite3_int64)rowid,
                                      spot->is_writable, &spot->pBlob);

    rc = convert_sqlite_error(idx, sqlite_rc);
    if (rc != DISKANN_OK) {
      spot->is_aborted = 1;
      spot->is_initialized = 0;
      return rc;
    }

    spot->rowid = rowid;
    spot->is_aborted = 0;
    spot->is_initialized = 0;
  }

  /* Reopen for different rowid if needed */
  if (spot->rowid != rowid) {
    int sqlite_rc = sqlite3_blob_reopen(spot->pBlob, (sqlite3_int64)rowid);
    rc = convert_sqlite_error(idx, sqlite_rc);
    if (rc != DISKANN_OK) {
      spot->is_aborted = 1;
      spot->is_initialized = 0;
      return rc;
    }
    spot->rowid = rowid;
    spot->is_initialized = 0;
  }

  /* Read BLOB data into buffer */
  int sqlite_rc = sqlite3_blob_read(spot->pBlob, spot->buffer, (int)buffer_size,
                                    0 /* offset */
  );

  if (sqlite_rc != SQLITE_OK) {
    spot->is_aborted = 1;
    spot->is_initialized = 0;
    return DISKANN_ERROR;
  }

  /* Success */
  idx->num_reads++;
  spot->is_initialized = 1;
  return DISKANN_OK;
}

int blob_spot_flush(DiskAnnIndex *idx, BlobSpot *spot) {
  /* Validate inputs */
  if (!idx || !spot) {
    return DISKANN_ERROR_INVALID;
  }
  if (!spot->is_writable) {
    return DISKANN_ERROR_INVALID;
  }
  /* Ensure buffer is initialized before writing (prevents writing uninitialized
   * memory) */
  if (!spot->is_initialized) {
    return DISKANN_ERROR_INVALID;
  }

  /* Write buffer to BLOB */
  int sqlite_rc = sqlite3_blob_write(spot->pBlob, spot->buffer,
                                     (int)spot->buffer_size, 0 /* offset */
  );

  if (sqlite_rc != SQLITE_OK) {
    return DISKANN_ERROR;
  }

  /* Update statistics */
  idx->num_writes++;
  return DISKANN_OK;
}

void blob_spot_free(BlobSpot *spot) {
  if (!spot) {
    return;
  }

  /* Close BLOB handle */
  if (spot->pBlob) {
    sqlite3_blob_close(spot->pBlob);
    spot->pBlob = NULL;
  }

  /* Free buffer */
  if (spot->buffer) {
    sqlite3_free(spot->buffer);
    spot->buffer = NULL;
  }

  /* Free structure */
  sqlite3_free(spot);
}
