/*
** DiskANN API implementation
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#include "diskann.h"
#include "diskann_blob.h"
#include "diskann_internal.h"
#include "diskann_node.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default configuration values */
#define DEFAULT_DIMENSIONS 768
#define DEFAULT_METRIC DISKANN_METRIC_EUCLIDEAN
#define DEFAULT_MAX_NEIGHBORS 32
#define DEFAULT_SEARCH_LIST_SIZE 100
#define DEFAULT_INSERT_LIST_SIZE 200
#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_PRUNING_ALPHA 1.2

/* Maximum allowed values */
#define MAX_DIMENSIONS 16384
#define MAX_BLOCK_SIZE 134217728 /* 128MB */
#define MAX_IDENTIFIER_LEN 64

/*
** Validate a SQL identifier (index name or database name).
** Must match [a-zA-Z_][a-zA-Z0-9_]*, max MAX_IDENTIFIER_LEN chars.
** Returns 1 if valid, 0 if invalid.
*/
static int validate_identifier(const char *name) {
  if (!name || !name[0])
    return 0;
  char c = name[0];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
    return 0;
  }
  size_t len = 1;
  for (const char *p = name + 1; *p; p++, len++) {
    if (len > MAX_IDENTIFIER_LEN)
      return 0;
    c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return 0;
    }
  }
  return 1;
}

/*
** Store a single metadata key-value pair as a SQLite INTEGER.
** Portable across platforms (no endianness issues).
*/
static int store_metadata_int(sqlite3 *db, const char *db_name,
                              const char *index_name, const char *key,
                              int64_t value) {
  /* index_name validated by caller (all callers run validate_identifier) */
  char *sql = sqlite3_mprintf(
      "INSERT OR REPLACE INTO \"%w\".%s_metadata (key, value) VALUES (?1, ?2)",
      db_name, index_name);
  if (!sql)
    return DISKANN_ERROR_NOMEM;

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK)
    return DISKANN_ERROR;

  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, value);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return (rc == SQLITE_DONE) ? DISKANN_OK : DISKANN_ERROR;
}

/*
** Check if a shadow table already exists for the given index.
** Returns 1 if exists, 0 if not, -1 on error.
*/
static int shadow_table_exists(sqlite3 *db, const char *db_name,
                               const char *index_name) {
  char *sql = sqlite3_mprintf("SELECT name FROM \"%w\".sqlite_master "
                              "WHERE type='table' AND name='%s_shadow'",
                              db_name, index_name);
  if (!sql)
    return -1;

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  if (rc != SQLITE_OK)
    return -1;

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_ROW) ? 1 : 0;
}

int diskann_create_index(sqlite3 *db, const char *db_name,
                         const char *index_name, const DiskAnnConfig *config) {
  char *sql = NULL;
  char *err_msg = NULL;
  int rc;

  /* Validate inputs */
  if (!db || !index_name || !db_name) {
    return DISKANN_ERROR_INVALID;
  }
  if (!validate_identifier(db_name) || !validate_identifier(index_name)) {
    return DISKANN_ERROR_INVALID;
  }

  /* Apply defaults if config is NULL */
  DiskAnnConfig default_config = {.dimensions = DEFAULT_DIMENSIONS,
                                  .metric = DEFAULT_METRIC,
                                  .max_neighbors = DEFAULT_MAX_NEIGHBORS,
                                  .search_list_size = DEFAULT_SEARCH_LIST_SIZE,
                                  .insert_list_size = DEFAULT_INSERT_LIST_SIZE,
                                  .block_size = DEFAULT_BLOCK_SIZE};

  if (!config) {
    config = &default_config;
  }

  /* Validate configuration */
  if (config->dimensions == 0 || config->dimensions > MAX_DIMENSIONS) {
    return DISKANN_ERROR_DIMENSION;
  }

  if (config->block_size == 0 || config->block_size > MAX_BLOCK_SIZE) {
    return DISKANN_ERROR_INVALID;
  }

  /* Fail if index already exists (don't silently overwrite config) */
  rc = shadow_table_exists(db, db_name, index_name);
  if (rc < 0)
    return DISKANN_ERROR;
  if (rc == 1)
    return DISKANN_ERROR_EXISTS;

  /* Wrap everything in a SAVEPOINT for atomicity */
  sql = sqlite3_mprintf("SAVEPOINT diskann_create_%s", index_name);
  if (!sql)
    return DISKANN_ERROR_NOMEM;
  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    if (err_msg)
      sqlite3_free(err_msg);
    return DISKANN_ERROR;
  }

  /* Create shadow table: {index_name}_shadow (id INTEGER PRIMARY KEY, data
   * BLOB) */
  sql = sqlite3_mprintf("CREATE TABLE \"%w\".%s_shadow ("
                        "  id INTEGER PRIMARY KEY,"
                        "  data BLOB NOT NULL"
                        ")",
                        db_name, index_name);

  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto rollback;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err_msg)
      sqlite3_free(err_msg);
    rc = DISKANN_ERROR;
    goto rollback;
  }

  /* Create metadata table: {index_name}_metadata (key TEXT, value INTEGER) */
  sql = sqlite3_mprintf("CREATE TABLE \"%w\".%s_metadata ("
                        "  key TEXT PRIMARY KEY,"
                        "  value INTEGER NOT NULL"
                        ")",
                        db_name, index_name);

  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto rollback;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err_msg)
      sqlite3_free(err_msg);
    rc = DISKANN_ERROR;
    goto rollback;
  }

  /* Store index configuration as portable integers */
  rc = store_metadata_int(db, db_name, index_name, "dimensions",
                          (int64_t)config->dimensions);
  if (rc != DISKANN_OK)
    goto rollback;
  rc = store_metadata_int(db, db_name, index_name, "metric",
                          (int64_t)config->metric);
  if (rc != DISKANN_OK)
    goto rollback;
  rc = store_metadata_int(db, db_name, index_name, "max_neighbors",
                          (int64_t)config->max_neighbors);
  if (rc != DISKANN_OK)
    goto rollback;
  rc = store_metadata_int(db, db_name, index_name, "search_list_size",
                          (int64_t)config->search_list_size);
  if (rc != DISKANN_OK)
    goto rollback;
  rc = store_metadata_int(db, db_name, index_name, "insert_list_size",
                          (int64_t)config->insert_list_size);
  if (rc != DISKANN_OK)
    goto rollback;
  rc = store_metadata_int(db, db_name, index_name, "block_size",
                          (int64_t)config->block_size);
  if (rc != DISKANN_OK)
    goto rollback;
  /* Store pruning_alpha as fixed-point integer (×1000) for portability */
  rc = store_metadata_int(db, db_name, index_name, "pruning_alpha_x1000",
                          (int64_t)(DEFAULT_PRUNING_ALPHA * 1000.0));
  if (rc != DISKANN_OK)
    goto rollback;

  /* Commit the savepoint */
  sql = sqlite3_mprintf("RELEASE diskann_create_%s", index_name);
  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto rollback;
  }
  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);
  if (rc != SQLITE_OK) {
    if (err_msg)
      sqlite3_free(err_msg);
    rc = DISKANN_ERROR;
    goto rollback;
  }

  return DISKANN_OK;

rollback:
  sql = sqlite3_mprintf("ROLLBACK TO diskann_create_%s; "
                        "RELEASE diskann_create_%s",
                        index_name, index_name);
  if (sql) {
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
  }
  return rc;
}

int diskann_open_index(sqlite3 *db, const char *db_name, const char *index_name,
                       DiskAnnIndex **out_index) {
  DiskAnnIndex *idx = NULL;
  char *sql = NULL;
  sqlite3_stmt *stmt = NULL;
  int rc = DISKANN_OK;

  /* Validate inputs */
  if (!db || !db_name || !index_name || !out_index) {
    return DISKANN_ERROR_INVALID;
  }
  if (!validate_identifier(db_name) || !validate_identifier(index_name)) {
    return DISKANN_ERROR_INVALID;
  }

  /* Initialize output to NULL */
  *out_index = NULL;

  /* Allocate index structure */
  idx = (DiskAnnIndex *)malloc(sizeof(DiskAnnIndex));
  if (!idx) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }
  memset(idx, 0, sizeof(DiskAnnIndex));

  /* Store database connection (borrowed reference) */
  idx->db = db;

  /* Duplicate strings (owned by this struct) */
  idx->db_name = sqlite3_mprintf("%s", db_name);
  idx->index_name = sqlite3_mprintf("%s", index_name);
  idx->shadow_name = sqlite3_mprintf("%s_shadow", index_name);

  if (!idx->db_name || !idx->index_name || !idx->shadow_name) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }

  /* Check if shadow table exists */
  sql = sqlite3_mprintf("SELECT name FROM \"%w\".sqlite_master WHERE "
                        "type='table' AND name='%s_shadow'",
                        db_name, index_name);

  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }

  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  sql = NULL;

  if (rc != SQLITE_OK) {
    rc = DISKANN_ERROR;
    goto cleanup;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (rc != SQLITE_ROW) {
    /* Shadow table doesn't exist */
    rc = DISKANN_ERROR_NOTFOUND;
    goto cleanup;
  }

  /* Load metadata from metadata table */
  sql = sqlite3_mprintf("SELECT key, value FROM \"%w\".%s_metadata", db_name,
                        index_name);

  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto cleanup;
  }

  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  sql = NULL;

  if (rc != SQLITE_OK) {
    /* Metadata table doesn't exist - index might be corrupted */
    rc = DISKANN_ERROR_NOTFOUND;
    goto cleanup;
  }

  /* Read all metadata entries (stored as portable integers) */
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *key = (const char *)sqlite3_column_text(stmt, 0);
    if (!key)
      continue;

    int64_t value = sqlite3_column_int64(stmt, 1);

    if (strcmp(key, "dimensions") == 0) {
      idx->dimensions = (uint32_t)value;
    } else if (strcmp(key, "metric") == 0) {
      idx->metric = (uint8_t)value;
    } else if (strcmp(key, "max_neighbors") == 0) {
      idx->max_neighbors = (uint32_t)value;
    } else if (strcmp(key, "search_list_size") == 0) {
      idx->search_list_size = (uint32_t)value;
    } else if (strcmp(key, "insert_list_size") == 0) {
      idx->insert_list_size = (uint32_t)value;
    } else if (strcmp(key, "block_size") == 0) {
      idx->block_size = (uint32_t)value;
    } else if (strcmp(key, "pruning_alpha_x1000") == 0) {
      idx->pruning_alpha = (double)value / 1000.0;
    }
  }

  sqlite3_finalize(stmt);
  stmt = NULL;

  /* Validate required metadata was loaded and within bounds */
  if (idx->dimensions == 0 || idx->dimensions > MAX_DIMENSIONS) {
    rc = DISKANN_ERROR;
    goto cleanup;
  }
  if (idx->block_size == 0 || idx->block_size > MAX_BLOCK_SIZE) {
    rc = DISKANN_ERROR;
    goto cleanup;
  }

  /* Compute derived layout fields (float32-only: both sizes identical) */
  idx->nNodeVectorSize = idx->dimensions * (uint32_t)sizeof(float);
  idx->nEdgeVectorSize = idx->nNodeVectorSize;

  /* Default pruning_alpha if not stored (backwards compat with old indexes) */
  if (idx->pruning_alpha == 0.0) {
    idx->pruning_alpha = DEFAULT_PRUNING_ALPHA;
  }

  /* Initialize statistics */
  idx->num_reads = 0;
  idx->num_writes = 0;

  /* Success - transfer ownership to caller */
  *out_index = idx;
  return DISKANN_OK;

cleanup:
  /* Error path - free allocated resources */
  if (stmt) {
    sqlite3_finalize(stmt);
  }
  if (sql) {
    sqlite3_free(sql);
  }
  if (idx) {
    diskann_close_index(idx);
  }
  return rc;
}

void diskann_close_index(DiskAnnIndex *idx) {
  if (!idx) {
    return; /* Safe to call with NULL */
  }

  /* Free malloc'd strings */
  if (idx->db_name) {
    sqlite3_free(idx->db_name);
    idx->db_name = NULL;
  }

  if (idx->index_name) {
    sqlite3_free(idx->index_name);
    idx->index_name = NULL;
  }

  if (idx->shadow_name) {
    sqlite3_free(idx->shadow_name);
    idx->shadow_name = NULL;
  }

  /* Note: idx->db is a borrowed reference - we don't close it */

  /* Free the index structure itself */
  free(idx);
}

/*
** Delete a vector from the index.
**
** Algorithm (conservative — no graph repair):
** 1. Load target node's BLOB to read its edge list
** 2. For each neighbor edge: load neighbor, find back-edge to target, remove it
** 3. Delete target row from shadow table
** Wrapped in SAVEPOINT for atomicity (modifies N+1 rows).
**
** Note: The original libSQL diskAnnDelete() has a bug at line 1676 — it
** searches for nodeBinEdgeFindIdx(neighbor, edgeRowid) where edgeRowid is
** the NEIGHBOR's own rowid. This should be the deleted node's rowid. Our
** implementation fixes this.
*/
int diskann_delete(DiskAnnIndex *idx, int64_t id) {
  BlobSpot *target_blob = NULL;
  BlobSpot *edge_blob = NULL;
  char *sql = NULL;
  sqlite3_stmt *stmt = NULL;
  int rc = DISKANN_OK;

  /* Validate inputs */
  if (!idx)
    return DISKANN_ERROR_INVALID;

  /* Begin SAVEPOINT for atomicity */
  sql = sqlite3_mprintf("SAVEPOINT diskann_delete_%s", idx->index_name);
  if (!sql)
    return DISKANN_ERROR_NOMEM;
  rc = sqlite3_exec(idx->db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);
  sql = NULL;
  if (rc != SQLITE_OK)
    return DISKANN_ERROR;

  /* Open read-only BlobSpot for target node */
  rc = blob_spot_create(idx, &target_blob, (uint64_t)id, idx->block_size,
                        DISKANN_BLOB_READONLY);
  if (rc == DISKANN_ROW_NOT_FOUND) {
    rc = DISKANN_ERROR_NOTFOUND;
    goto rollback;
  }
  if (rc != DISKANN_OK)
    goto rollback;

  /* Read target node data */
  rc = blob_spot_reload(idx, target_blob, (uint64_t)id, idx->block_size);
  if (rc != DISKANN_OK)
    goto rollback;

  /* Read edge count and clean up back-edges from neighbors */
  uint16_t n_edges = node_bin_edges(idx, target_blob);

  if (n_edges > 0) {
    /* Create writable BlobSpot (initial rowid = target, which exists) */
    rc = blob_spot_create(idx, &edge_blob, (uint64_t)id, idx->block_size,
                          DISKANN_BLOB_WRITABLE);
    if (rc != DISKANN_OK)
      goto rollback;

    for (uint16_t i = 0; i < n_edges; i++) {
      uint64_t edge_rowid;
      node_bin_edge(idx, target_blob, (int)i, &edge_rowid, NULL, NULL);

      rc = blob_spot_reload(idx, edge_blob, edge_rowid, idx->block_size);
      if (rc == DISKANN_ROW_NOT_FOUND) {
        continue; /* Zombie edge — neighbor already deleted */
      }
      if (rc != DISKANN_OK)
        goto rollback;

      /* Find back-edge pointing to the deleted node (NOT the neighbor's own id)
       */
      int del_idx = node_bin_edge_find_idx(idx, edge_blob, (uint64_t)id);
      if (del_idx == -1) {
        continue; /* No back-edge (unidirectional or already removed) */
      }

      node_bin_delete_edge(idx, edge_blob, del_idx);
      rc = blob_spot_flush(idx, edge_blob);
      if (rc != DISKANN_OK)
        goto rollback;
    }

    blob_spot_free(edge_blob);
    edge_blob = NULL;
  }

  /* Free target blob before deleting the row */
  blob_spot_free(target_blob);
  target_blob = NULL;

  /* Delete shadow table row */
  sql = sqlite3_mprintf("DELETE FROM \"%w\".%s WHERE id = ?", idx->db_name,
                        idx->shadow_name);
  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto rollback;
  }

  rc = sqlite3_prepare_v2(idx->db, sql, -1, &stmt, NULL);
  sqlite3_free(sql);
  sql = NULL;
  if (rc != SQLITE_OK) {
    rc = DISKANN_ERROR;
    goto rollback;
  }

  sqlite3_bind_int64(stmt, 1, id);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = NULL;

  if (rc != SQLITE_DONE) {
    rc = DISKANN_ERROR;
    goto rollback;
  }

  /* Verify exactly 1 row was deleted */
  if (sqlite3_changes(idx->db) != 1) {
    rc = DISKANN_ERROR_NOTFOUND;
    goto rollback;
  }

  /* Release SAVEPOINT — commit */
  sql = sqlite3_mprintf("RELEASE diskann_delete_%s", idx->index_name);
  if (!sql) {
    rc = DISKANN_ERROR_NOMEM;
    goto rollback;
  }
  rc = sqlite3_exec(idx->db, sql, NULL, NULL, NULL);
  sqlite3_free(sql);

  return (rc == SQLITE_OK) ? DISKANN_OK : DISKANN_ERROR;

rollback:
  if (target_blob)
    blob_spot_free(target_blob);
  if (edge_blob)
    blob_spot_free(edge_blob);
  if (stmt)
    sqlite3_finalize(stmt);

  sql = sqlite3_mprintf("ROLLBACK TO diskann_delete_%s; "
                        "RELEASE diskann_delete_%s",
                        idx->index_name, idx->index_name);
  if (sql) {
    sqlite3_exec(idx->db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
  }
  return rc;
}

int diskann_drop_index(sqlite3 *db, const char *db_name,
                       const char *index_name) {
  char *sql = NULL;
  char *err_msg = NULL;
  int rc;

  /* Validate inputs */
  if (!db || !index_name || !db_name) {
    return DISKANN_ERROR_INVALID;
  }
  if (!validate_identifier(db_name) || !validate_identifier(index_name)) {
    return DISKANN_ERROR_INVALID;
  }

  /* Check if shadow table exists using helper */
  rc = shadow_table_exists(db, db_name, index_name);
  if (rc < 0) {
    return DISKANN_ERROR;
  }
  if (rc == 0) {
    return DISKANN_ERROR_NOTFOUND;
  }

  /* Drop the shadow table */
  sql = sqlite3_mprintf("DROP TABLE \"%w\".%s_shadow", db_name, index_name);

  if (!sql) {
    return DISKANN_ERROR_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return DISKANN_ERROR;
  }

  /* Also drop the metadata table */
  sql = sqlite3_mprintf("DROP TABLE IF EXISTS \"%w\".%s_metadata", db_name,
                        index_name);

  if (!sql) {
    return DISKANN_ERROR_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return DISKANN_ERROR;
  }

  return DISKANN_OK;
}

int diskann_clear_index(sqlite3 *db, const char *db_name,
                        const char *index_name) {
  char *sql = NULL;
  char *err_msg = NULL;
  int rc;

  /* Validate inputs */
  if (!db || !index_name || !db_name) {
    return DISKANN_ERROR_INVALID;
  }
  if (!validate_identifier(db_name) || !validate_identifier(index_name)) {
    return DISKANN_ERROR_INVALID;
  }

  /* Check if shadow table exists using helper */
  rc = shadow_table_exists(db, db_name, index_name);
  if (rc < 0) {
    return DISKANN_ERROR;
  }
  if (rc == 0) {
    return DISKANN_ERROR_NOTFOUND;
  }

  /* Delete all rows from shadow table */
  sql = sqlite3_mprintf("DELETE FROM \"%w\".%s_shadow", db_name, index_name);

  if (!sql) {
    return DISKANN_ERROR_NOMEM;
  }

  rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
  sqlite3_free(sql);

  if (rc != SQLITE_OK) {
    if (err_msg) {
      sqlite3_free(err_msg);
    }
    return DISKANN_ERROR;
  }

  return DISKANN_OK;
}
