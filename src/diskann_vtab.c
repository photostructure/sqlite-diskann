/*
** Virtual table implementation for DiskANN
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** Wraps the DiskANN C API as a SQLite virtual table.
** Supports CREATE, INSERT, SELECT (MATCH search), DELETE, DROP.
**
** Schema: CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)
** All columns HIDDEN. rowid via xRowid. MATCH on vector col for ANN search.
**
** Usage:
**   CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean);
**   INSERT INTO t(rowid, vector) VALUES (1, X'...');
**   SELECT rowid, distance FROM t WHERE vector MATCH ?query AND k = 10;
**   DELETE FROM t WHERE rowid = 1;
**   DROP TABLE t;
*/

/* Mark this as the main file that defines sqlite3_api (not extern).
** Other .c files that include diskann_sqlite.h will get extern declaration.
** This is required for multi-file extensions - do not remove. */
#define DISKANN_VTAB_MAIN

#include "diskann.h"
#include "diskann_internal.h"
#include "diskann_sqlite.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SQLITE_EXTENSION_INIT1 must appear in exactly ONE source file */
#ifdef DISKANN_EXTENSION
SQLITE_EXTENSION_INIT1
#endif

/* xBestIndex idxNum bitmask */
#define DISKANN_IDX_MATCH 0x01
#define DISKANN_IDX_K 0x02
#define DISKANN_IDX_LIMIT 0x04
#define DISKANN_IDX_ROWID 0x08

/* Column indices in the vtab schema */
#define DISKANN_COL_VECTOR 0
#define DISKANN_COL_DISTANCE 1
#define DISKANN_COL_K 2

/* Virtual table structure */
typedef struct diskann_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *db_name;
  char *table_name;
  DiskAnnIndex *idx;   /* Opened index (kept open for performance) */
  uint32_t dimensions; /* Cached from idx for dim validation in xUpdate */
} diskann_vtab;

/* Cursor structure for iteration */
typedef struct diskann_cursor {
  sqlite3_vtab_cursor base;
  DiskAnnResult *results; /* Search results (sqlite3_malloc'd) */
  int num_results;        /* Actual count from diskann_search() */
  int current;            /* Current position (0-based) */
} diskann_cursor;

/* Forward declarations */
static int diskannCreate(sqlite3 *db, void *pAux, int argc,
                         const char *const *argv, sqlite3_vtab **ppVtab,
                         char **pzErr);
static int diskannConnect(sqlite3 *db, void *pAux, int argc,
                          const char *const *argv, sqlite3_vtab **ppVtab,
                          char **pzErr);
static int diskannDisconnect(sqlite3_vtab *pVtab);
static int diskannDestroy(sqlite3_vtab *pVtab);
static int diskannBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo);
static int diskannOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor);
static int diskannClose(sqlite3_vtab_cursor *pCursor);
static int diskannFilter(sqlite3_vtab_cursor *pCursor, int idxNum,
                         const char *idxStr, int argc, sqlite3_value **argv);
static int diskannNext(sqlite3_vtab_cursor *pCursor);
static int diskannEof(sqlite3_vtab_cursor *pCursor);
static int diskannColumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                         int i);
static int diskannRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid);
static int diskannUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
                         sqlite_int64 *pRowid);
static int diskannShadowName(const char *zName);

/*
** Parse metric string to enum. Returns -1 on unknown metric.
*/
static int parse_metric(const char *str) {
  if (strcmp(str, "cosine") == 0)
    return DISKANN_METRIC_COSINE;
  if (strcmp(str, "euclidean") == 0)
    return DISKANN_METRIC_EUCLIDEAN;
  if (strcmp(str, "dot") == 0)
    return DISKANN_METRIC_DOT;
  return -1;
}

/*
** Shared init helper for xCreate and xConnect.
** Declares the vtab schema, allocates the vtab struct, populates fields.
*/
static int vtab_init(sqlite3 *db, const char *db_name, const char *table_name,
                     DiskAnnIndex *idx, sqlite3_vtab **ppVtab, char **pzErr) {
  int rc;

  rc = sqlite3_declare_vtab(
      db, "CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN)");
  if (rc != SQLITE_OK) {
    *pzErr = sqlite3_mprintf("diskann: declare_vtab failed");
    return rc;
  }

  diskann_vtab *pVtab = sqlite3_malloc(sizeof(*pVtab));
  if (!pVtab) {
    return SQLITE_NOMEM;
  }
  memset(pVtab, 0, sizeof(*pVtab));

  pVtab->db = db;
  pVtab->db_name = sqlite3_mprintf("%s", db_name);
  pVtab->table_name = sqlite3_mprintf("%s", table_name);
  pVtab->idx = idx;
  pVtab->dimensions = idx->dimensions;

  if (!pVtab->db_name || !pVtab->table_name) {
    sqlite3_free(pVtab->db_name);
    sqlite3_free(pVtab->table_name);
    sqlite3_free(pVtab);
    return SQLITE_NOMEM;
  }

  *ppVtab = &pVtab->base;
  return SQLITE_OK;
}

/*
** xCreate — called for CREATE VIRTUAL TABLE.
** Parses config from argv, creates shadow tables, opens index.
*/
static int diskannCreate(sqlite3 *db, void *pAux, int argc,
                         const char *const *argv, sqlite3_vtab **ppVtab,
                         char **pzErr) {
  DiskAnnConfig config;
  DiskAnnIndex *idx = NULL;
  int rc;

  (void)pAux;

  /* Default configuration */
  config.dimensions = 0; /* Required */
  config.metric = DISKANN_METRIC_COSINE;
  config.max_neighbors = 64;
  config.search_list_size = 100;
  config.insert_list_size = 200;
  config.block_size = 4096;

  if (argc < 3) {
    *pzErr = sqlite3_mprintf("diskann: missing arguments");
    return SQLITE_ERROR;
  }

  const char *db_name = argv[1];
  const char *table_name = argv[2];

  /* Parse CREATE VIRTUAL TABLE parameters */
  for (int i = 3; i < argc; i++) {
    const char *param = argv[i];
    char key[64], value[64];

    if (sscanf(param, "%63[^=]=%63s", key, value) == 2) {
      if (strcmp(key, "dimension") == 0) {
        config.dimensions = (uint32_t)atoi(value);
      } else if (strcmp(key, "metric") == 0) {
        int metric = parse_metric(value);
        if (metric < 0) {
          *pzErr = sqlite3_mprintf("diskann: invalid metric '%s'", value);
          return SQLITE_ERROR;
        }
        config.metric = (uint8_t)metric;
      } else if (strcmp(key, "max_degree") == 0) {
        config.max_neighbors = (uint32_t)atoi(value);
      } else if (strcmp(key, "build_search_list_size") == 0) {
        config.search_list_size = (uint32_t)atoi(value);
        config.insert_list_size = config.search_list_size * 2;
      }
    }
  }

  if (config.dimensions == 0) {
    *pzErr = sqlite3_mprintf("diskann: dimension parameter required");
    return SQLITE_ERROR;
  }

  /* Create index (shadow tables + metadata) */
  rc = diskann_create_index(db, db_name, table_name, &config);
  if (rc != DISKANN_OK && rc != DISKANN_ERROR_EXISTS) {
    *pzErr = sqlite3_mprintf("diskann: failed to create index (rc=%d)", rc);
    return SQLITE_ERROR;
  }

  /* Open the index */
  rc = diskann_open_index(db, db_name, table_name, &idx);
  if (rc != DISKANN_OK) {
    *pzErr = sqlite3_mprintf("diskann: failed to open index (rc=%d)", rc);
    return SQLITE_ERROR;
  }

  rc = vtab_init(db, db_name, table_name, idx, ppVtab, pzErr);
  if (rc != SQLITE_OK) {
    diskann_close_index(idx);
    return rc;
  }

  return SQLITE_OK;
}

/*
** xConnect — called when an existing vtab is reconnected (e.g., after reopen).
** Opens the existing index — does NOT parse config (config from metadata).
*/
static int diskannConnect(sqlite3 *db, void *pAux, int argc,
                          const char *const *argv, sqlite3_vtab **ppVtab,
                          char **pzErr) {
  DiskAnnIndex *idx = NULL;
  int rc;

  (void)pAux;
  (void)argc;

  if (argc < 3) {
    *pzErr = sqlite3_mprintf("diskann: missing arguments");
    return SQLITE_ERROR;
  }

  const char *db_name = argv[1];
  const char *table_name = argv[2];

  /* Open existing index — config comes from persisted metadata */
  rc = diskann_open_index(db, db_name, table_name, &idx);
  if (rc != DISKANN_OK) {
    *pzErr = sqlite3_mprintf("diskann: index not found (rc=%d)", rc);
    return SQLITE_ERROR;
  }

  rc = vtab_init(db, db_name, table_name, idx, ppVtab, pzErr);
  if (rc != SQLITE_OK) {
    diskann_close_index(idx);
    return rc;
  }

  return SQLITE_OK;
}

/*
** xDisconnect — close index handle, free vtab.
*/
static int diskannDisconnect(sqlite3_vtab *pVtab) {
  diskann_vtab *p = (diskann_vtab *)pVtab;
  diskann_close_index(p->idx);
  sqlite3_free(p->db_name);
  sqlite3_free(p->table_name);
  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** xDestroy — drop shadow tables, close index, free vtab.
** Called on DROP TABLE.
*/
static int diskannDestroy(sqlite3_vtab *pVtab) {
  diskann_vtab *p = (diskann_vtab *)pVtab;

  /* Close index first (releases blob handles before DROP) */
  diskann_close_index(p->idx);
  p->idx = NULL;

  /* Drop shadow tables */
  diskann_drop_index(p->db, p->db_name, p->table_name);

  sqlite3_free(p->db_name);
  sqlite3_free(p->table_name);
  sqlite3_free(p);
  return SQLITE_OK;
}

/*
** xBestIndex — query planning.
** Recognizes MATCH (vector search), EQ on k, LIMIT, and ROWID EQ.
*/
static int diskannBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  int idxNum = 0;

  (void)pVtab;

  /* Pass 1: Find constraint positions.
  ** SQLite presents constraints in arbitrary order, but xFilter reads argv
  ** in a fixed order (MATCH, K, LIMIT, ROWID). We must assign argvIndex
  ** values that match xFilter's consumption order, not constraint array
  ** order. Record positions first, assign in pass 2. */
  int i_match = -1, i_k = -1, i_limit = -1, i_rowid = -1;

  for (int i = 0; i < pInfo->nConstraint; i++) {
    struct sqlite3_index_constraint *c = &pInfo->aConstraint[i];
    if (!c->usable)
      continue;

    if (c->op == SQLITE_INDEX_CONSTRAINT_MATCH &&
        c->iColumn == DISKANN_COL_VECTOR) {
      i_match = i;
      idxNum |= DISKANN_IDX_MATCH;
    } else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ &&
               c->iColumn == DISKANN_COL_K) {
      i_k = i;
      idxNum |= DISKANN_IDX_K;
    } else if (c->op == SQLITE_INDEX_CONSTRAINT_LIMIT) {
      i_limit = i;
      idxNum |= DISKANN_IDX_LIMIT;
    } else if (c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn == -1) {
      i_rowid = i;
      idxNum |= DISKANN_IDX_ROWID;
    }
  }

  /* Pass 2: Assign argvIndex in the order xFilter consumes them.
  ** This ensures argv[0] = MATCH, argv[1] = K, argv[2] = LIMIT, etc.
  ** regardless of the order SQLite presented constraints. */
  int next_argv = 1;
  if (i_match >= 0) {
    pInfo->aConstraintUsage[i_match].argvIndex = next_argv++;
    pInfo->aConstraintUsage[i_match].omit = 1;
  }
  if (i_k >= 0) {
    pInfo->aConstraintUsage[i_k].argvIndex = next_argv++;
    pInfo->aConstraintUsage[i_k].omit = 1;
  }
  if (i_limit >= 0) {
    pInfo->aConstraintUsage[i_limit].argvIndex = next_argv++;
    pInfo->aConstraintUsage[i_limit].omit = 1;
  }
  if (i_rowid >= 0) {
    pInfo->aConstraintUsage[i_rowid].argvIndex = next_argv++;
    pInfo->aConstraintUsage[i_rowid].omit = 1;
  }

  pInfo->idxNum = idxNum;

  if (idxNum & DISKANN_IDX_MATCH) {
    pInfo->estimatedCost = 100.0;
    pInfo->estimatedRows = 10;
  } else if (idxNum & DISKANN_IDX_ROWID) {
    pInfo->estimatedCost = 1.0;
    pInfo->estimatedRows = 1;
    pInfo->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
  } else {
    pInfo->estimatedCost = 1e12;
    pInfo->estimatedRows = 0;
  }

  return SQLITE_OK;
}

/*
** xOpen — allocate a cursor.
*/
static int diskannOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
  diskann_cursor *pCur;

  (void)pVtab;

  pCur = sqlite3_malloc(sizeof(*pCur));
  if (!pCur) {
    return SQLITE_NOMEM;
  }
  memset(pCur, 0, sizeof(*pCur));

  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
** xClose — free cursor and results.
*/
static int diskannClose(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  sqlite3_free(pCur->results);
  sqlite3_free(pCur);
  return SQLITE_OK;
}

/*
** xFilter — execute search or ROWID lookup based on idxNum from xBestIndex.
*/
static int diskannFilter(sqlite3_vtab_cursor *pCursor, int idxNum,
                         const char *idxStr, int argc, sqlite3_value **argv) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  diskann_vtab *pVtab = (diskann_vtab *)pCursor->pVtab;
  int next = 0;

  (void)idxStr;
  (void)argc;

  /* Free previous results */
  if (pCur->results) {
    sqlite3_free(pCur->results);
    pCur->results = NULL;
  }
  pCur->num_results = 0;
  pCur->current = 0;

  if (idxNum & DISKANN_IDX_MATCH) {
    /* ANN search path */
    const float *query = (const float *)sqlite3_value_blob(argv[next]);
    int bytes = sqlite3_value_bytes(argv[next]);
    uint32_t query_dims = (uint32_t)((size_t)bytes / sizeof(float));
    next++;

    int k = 10; /* default */
    if (idxNum & DISKANN_IDX_K) {
      k = sqlite3_value_int(argv[next]);
      if (k <= 0)
        k = 10;
      next++;
    }
    if (idxNum & DISKANN_IDX_LIMIT) {
      int limit = sqlite3_value_int(argv[next]);
      if (limit > 0 && limit < k)
        k = limit;
      next++;
    }
    /* Skip ROWID arg if also present (unlikely with MATCH, but be safe) */
    if (idxNum & DISKANN_IDX_ROWID) {
      next++;
    }

    if (!query || query_dims == 0) {
      pCur->num_results = 0;
      return SQLITE_OK;
    }

    pCur->results = sqlite3_malloc(k * (int)sizeof(DiskAnnResult));
    if (!pCur->results)
      return SQLITE_NOMEM;

    int rc = diskann_search(pVtab->idx, query, query_dims, k, pCur->results);
    if (rc < 0) {
      sqlite3_free(pCur->results);
      pCur->results = NULL;
      return SQLITE_ERROR;
    }
    pCur->num_results = rc; /* rc IS the count */
    pCur->current = 0;
    return SQLITE_OK;
  }

  if (idxNum & DISKANN_IDX_ROWID) {
    /* ROWID scan — single-row lookup for DELETE support */
    sqlite_int64 target = sqlite3_value_int64(argv[next]);

    /* Check if row exists in shadow table */
    char *sql =
        sqlite3_mprintf("SELECT 1 FROM \"%w\".\"%w_shadow\" WHERE id = ?",
                        pVtab->db_name, pVtab->table_name);
    if (!sql)
      return SQLITE_NOMEM;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(pVtab->db, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
      return rc;

    sqlite3_bind_int64(stmt, 1, target);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      pCur->results = sqlite3_malloc((int)sizeof(DiskAnnResult));
      if (!pCur->results) {
        sqlite3_finalize(stmt);
        return SQLITE_NOMEM;
      }
      pCur->results[0].id = target;
      pCur->results[0].distance = 0.0f;
      pCur->num_results = 1;
    } else {
      pCur->num_results = 0;
    }
    sqlite3_finalize(stmt);
    pCur->current = 0;
    return SQLITE_OK;
  }

  /* No MATCH, no ROWID → empty result set */
  pCur->num_results = 0;
  return SQLITE_OK;
}

/*
** xNext — advance cursor to next result.
*/
static int diskannNext(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  pCur->current++;
  return SQLITE_OK;
}

/*
** xEof — check if cursor is at end.
*/
static int diskannEof(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  return pCur->current >= pCur->num_results;
}

/*
** xColumn — return column value.
** Col 0 = vector (NULL, write-only in search context)
** Col 1 = distance
** Col 2 = k (NULL, input-only parameter)
*/
static int diskannColumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                         int i) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;

  if (pCur->current >= pCur->num_results)
    return SQLITE_ERROR;

  switch (i) {
  case DISKANN_COL_VECTOR:
    sqlite3_result_null(ctx);
    break;
  case DISKANN_COL_DISTANCE:
    sqlite3_result_double(ctx, (double)pCur->results[pCur->current].distance);
    break;
  case DISKANN_COL_K:
    sqlite3_result_null(ctx);
    break;
  default:
    sqlite3_result_null(ctx);
    break;
  }

  return SQLITE_OK;
}

/*
** xRowid — return current row's rowid.
*/
static int diskannRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;

  if (pCur->current >= pCur->num_results)
    return SQLITE_ERROR;

  *pRowid = pCur->results[pCur->current].id;
  return SQLITE_OK;
}

/*
** xUpdate — INSERT, DELETE handler.
**
** INSERT: argv[0]=NULL, argv[1]=rowid, argv[2]=vector, argv[3]=distance(NULL),
**         argv[4]=k(NULL). argc = 2 + nColumns = 5.
** DELETE: argv[0]=rowid. argc = 1.
*/
static int diskannUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
                         sqlite_int64 *pRowid) {
  diskann_vtab *p = (diskann_vtab *)pVtab;

  if (argc == 1) {
    /* DELETE */
    sqlite_int64 rowid = sqlite3_value_int64(argv[0]);
    int rc = diskann_delete(p->idx, rowid);
    /* NOTFOUND is not an error for DELETE — row may already be gone */
    if (rc == DISKANN_OK || rc == DISKANN_ERROR_NOTFOUND)
      return SQLITE_OK;
    pVtab->zErrMsg = sqlite3_mprintf("diskann: delete failed (rc=%d)", rc);
    return SQLITE_ERROR;
  }

  if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    /* INSERT */
    if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: rowid required for INSERT");
      return SQLITE_ERROR;
    }
    sqlite_int64 rowid = sqlite3_value_int64(argv[1]);

    /* argv[2] = col 0 = vector (BLOB) */
    if (sqlite3_value_type(argv[2]) != SQLITE_BLOB) {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: vector must be a BLOB");
      return SQLITE_ERROR;
    }

    const float *vec = (const float *)sqlite3_value_blob(argv[2]);
    int bytes = sqlite3_value_bytes(argv[2]);
    uint32_t dims = (uint32_t)((size_t)bytes / sizeof(float));

    if (dims != p->dimensions) {
      pVtab->zErrMsg =
          sqlite3_mprintf("diskann: dimension mismatch (got %u, expected %u)",
                          dims, p->dimensions);
      return SQLITE_ERROR;
    }

    /* argv[3]=distance(NULL), argv[4]=k(NULL) — skip */
    int rc = diskann_insert(p->idx, rowid, vec, dims);
    if (rc == DISKANN_OK) {
      *pRowid = rowid;
      return SQLITE_OK;
    }
    pVtab->zErrMsg = sqlite3_mprintf("diskann: insert failed (rc=%d)", rc);
    return SQLITE_ERROR;
  }

  /* UPDATE — not supported */
  pVtab->zErrMsg = sqlite3_mprintf("diskann: UPDATE not supported");
  return SQLITE_ERROR;
}

/*
** xShadowName — protect shadow tables from direct manipulation.
*/
static int diskannShadowName(const char *zName) {
  return sqlite3_stricmp(zName, "shadow") == 0 ||
         sqlite3_stricmp(zName, "metadata") == 0;
}

/*
** Virtual table module definition (iVersion=3 for xShadowName)
*/
static sqlite3_module diskannModule = {
    3,                 /* iVersion — enables xShadowName */
    diskannCreate,     /* xCreate — creates shadow tables + opens index */
    diskannConnect,    /* xConnect — opens existing index only */
    diskannBestIndex,  /* xBestIndex */
    diskannDisconnect, /* xDisconnect — closes index handle */
    diskannDestroy,    /* xDestroy — drops shadow tables + frees */
    diskannOpen,       /* xOpen */
    diskannClose,      /* xClose */
    diskannFilter,     /* xFilter */
    diskannNext,       /* xNext */
    diskannEof,        /* xEof */
    diskannColumn,     /* xColumn */
    diskannRowid,      /* xRowid */
    diskannUpdate,     /* xUpdate */
    NULL,              /* xBegin */
    NULL,              /* xSync */
    NULL,              /* xCommit */
    NULL,              /* xRollback */
    NULL,              /* xFindFunction */
    NULL,              /* xRename */
    NULL,              /* xSavepoint */
    NULL,              /* xRelease */
    NULL,              /* xRollbackTo */
    diskannShadowName, /* xShadowName */
    NULL,              /* xIntegrity */
};

/*
** Register the diskann virtual table module.
** Called as extension entry point or directly from test code.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_diskann_init(sqlite3 *db, char **pzErrMsg,
                             const sqlite3_api_routines *pApi) {
  int rc;

#ifdef DISKANN_EXTENSION
  SQLITE_EXTENSION_INIT2(pApi);
#else
  (void)pApi; /* Unused in test builds */
#endif

  rc = sqlite3_create_module_v2(db, "diskann", &diskannModule, NULL, NULL);

  if (rc != SQLITE_OK) {
    if (pzErrMsg) {
      *pzErrMsg = sqlite3_mprintf(
          "diskann_init: sqlite3_create_module_v2 failed with rc=%d", rc);
    }
    return rc;
  }

  return SQLITE_OK;
}
