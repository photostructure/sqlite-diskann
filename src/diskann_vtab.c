/*
** Virtual table implementation for DiskANN
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** Minimal virtual table that wraps the DiskANN C API.
** Supports CREATE, INSERT, SELECT (search), DELETE.
*/

/* Mark this as the main file that defines sqlite3_api (not extern) */
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

/* Virtual table structure */
typedef struct diskann_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *db_name;
  char *table_name;
  DiskAnnIndex *idx; /* Opened index (kept open for performance) */
} diskann_vtab;

/* Cursor structure for iteration */
typedef struct diskann_cursor {
  sqlite3_vtab_cursor base;
  DiskAnnResult *results; /* Search results */
  int num_results;        /* Number of results */
  int current;            /* Current position in results */
} diskann_cursor;

/* Forward declarations */
static int diskannConnect(sqlite3 *db, void *pAux, int argc,
                          const char *const *argv, sqlite3_vtab **ppVtab,
                          char **pzErr);
static int diskannDisconnect(sqlite3_vtab *pVtab);
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

/*
** Parse metric string to enum
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
** CREATE VIRTUAL TABLE handler
**
** Syntax:
**   CREATE VIRTUAL TABLE name USING diskann(
**     dimension=128,
**     metric=cosine,
**     max_degree=64,
**     build_search_list_size=100,
**     normalize_vectors=0
**   )
*/
static int diskannConnect(sqlite3 *db, void *pAux, int argc,
                          const char *const *argv, sqlite3_vtab **ppVtab,
                          char **pzErr) {
  diskann_vtab *pVtab;
  DiskAnnConfig config;
  int rc;

  /* Default configuration */
  config.dimensions = 0; /* Required */
  config.metric = DISKANN_METRIC_COSINE;
  config.max_neighbors = 64;
  config.search_list_size = 100;
  config.insert_list_size = 200;
  config.block_size = 4096;

  (void)pAux;

  /* Parse arguments: argv[0]=module, argv[1]=db, argv[2]=table,
   * argv[3..]=params
   */
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
      /* Ignore normalize_vectors for now - not part of C API */
    }
  }

  if (config.dimensions == 0) {
    *pzErr = sqlite3_mprintf("diskann: dimension parameter required");
    return SQLITE_ERROR;
  }

  /* Create index */
  rc = diskann_create_index(db, db_name, table_name, &config);
  if (rc != DISKANN_OK && rc != DISKANN_ERROR_EXISTS) {
    *pzErr = sqlite3_mprintf("diskann: failed to create index");
    return SQLITE_ERROR;
  }

  /* Allocate vtab structure */
  pVtab = sqlite3_malloc(sizeof(*pVtab));
  if (!pVtab) {
    return SQLITE_NOMEM;
  }
  memset(pVtab, 0, sizeof(*pVtab));

  pVtab->db = db;
  pVtab->db_name = sqlite3_mprintf("%s", db_name);
  pVtab->table_name = sqlite3_mprintf("%s", table_name);

  /* Open the index */
  rc = diskann_open_index(db, db_name, table_name, &pVtab->idx);
  if (rc != DISKANN_OK) {
    sqlite3_free(pVtab->db_name);
    sqlite3_free(pVtab->table_name);
    sqlite3_free(pVtab);
    *pzErr = sqlite3_mprintf("diskann: failed to open index");
    return SQLITE_ERROR;
  }

  /* Declare schema: (rowid INTEGER PRIMARY KEY, vector BLOB) */
  rc = sqlite3_declare_vtab(
      db, "CREATE TABLE x(rowid INTEGER PRIMARY KEY, vector BLOB)");
  if (rc != SQLITE_OK) {
    diskann_close_index(pVtab->idx);
    sqlite3_free(pVtab->db_name);
    sqlite3_free(pVtab->table_name);
    sqlite3_free(pVtab);
    return rc;
  }

  *ppVtab = &pVtab->base;
  return SQLITE_OK;
}

/*
** Disconnect from virtual table
*/
static int diskannDisconnect(sqlite3_vtab *pVtab) {
  diskann_vtab *p = (diskann_vtab *)pVtab;

  if (p->idx) {
    diskann_close_index(p->idx);
  }
  sqlite3_free(p->db_name);
  sqlite3_free(p->table_name);
  sqlite3_free(p);

  return SQLITE_OK;
}

/*
** Best index selection for query planning
**
** We support:
** 1. Full table scan (not useful for ANN)
** 2. Search by vector (ANN search)
*/
static int diskannBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  (void)pVtab;

  /* Check if vector column is constrained (for search) */
  for (int i = 0; i < pInfo->nConstraint; i++) {
    if (pInfo->aConstraint[i].iColumn == 1 && /* vector column */
        pInfo->aConstraint[i].usable &&
        pInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ) {
      /* Vector equality constraint - use for ANN search */
      pInfo->aConstraintUsage[i].argvIndex = 1;
      pInfo->aConstraintUsage[i].omit = 1;
      pInfo->idxNum = 1; /* ANN search */
      pInfo->estimatedCost = 100.0;
      pInfo->estimatedRows = 10;
      return SQLITE_OK;
    }
  }

  /* Full table scan - very expensive */
  pInfo->estimatedCost = 1000000.0;
  pInfo->estimatedRows = 100000;
  pInfo->idxNum = 0;

  return SQLITE_OK;
}

/*
** Open a cursor
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
** Close a cursor
*/
static int diskannClose(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;

  if (pCur->results) {
    sqlite3_free(pCur->results);
  }
  sqlite3_free(pCur);

  return SQLITE_OK;
}

/*
** Filter/search operation
**
** idxNum: 0=full scan, 1=ANN search
** argc==1 && argv[0] = query vector BLOB
*/
static int diskannFilter(sqlite3_vtab_cursor *pCursor, int idxNum,
                         const char *idxStr, int argc, sqlite3_value **argv) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  diskann_vtab *pVtab = (diskann_vtab *)pCursor->pVtab;
  int k = 10; /* Default: return 10 neighbors */
  int rc;

  (void)idxStr;

  /* Free previous results */
  if (pCur->results) {
    sqlite3_free(pCur->results);
    pCur->results = NULL;
  }
  pCur->num_results = 0;
  pCur->current = 0;

  if (idxNum == 1 && argc == 1) {
    /* ANN search */
    const void *query_blob = sqlite3_value_blob(argv[0]);
    int blob_bytes = sqlite3_value_bytes(argv[0]);
    uint32_t query_dims = (uint32_t)((size_t)blob_bytes / sizeof(float));

    if (!query_blob || query_dims == 0) {
      return SQLITE_ERROR;
    }

    /* Allocate result buffer */
    pCur->results = sqlite3_malloc((int)((size_t)k * sizeof(DiskAnnResult)));
    if (!pCur->results) {
      return SQLITE_NOMEM;
    }

    /* Perform search */
    rc = diskann_search(pVtab->idx, (const float *)query_blob, query_dims, k,
                        pCur->results);
    if (rc < 0) {
      sqlite3_free(pCur->results);
      pCur->results = NULL;
      return SQLITE_ERROR;
    }

    pCur->num_results = rc;
    pCur->current = 0;
  } else {
    /* Full table scan - not supported */
    pCur->num_results = 0;
  }

  return SQLITE_OK;
}

/*
** Advance cursor to next result
*/
static int diskannNext(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  pCur->current++;
  return SQLITE_OK;
}

/*
** Check if cursor is at end
*/
static int diskannEof(sqlite3_vtab_cursor *pCursor) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;
  return pCur->current >= pCur->num_results;
}

/*
** Return column value
**
** Column 0: rowid
** Column 1: vector (NULL - not returned in search results)
*/
static int diskannColumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx,
                         int i) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;

  if (pCur->current >= pCur->num_results) {
    return SQLITE_ERROR;
  }

  if (i == 0) {
    /* rowid */
    sqlite3_result_int64(ctx, pCur->results[pCur->current].id);
  } else if (i == 1) {
    /* vector - not available in search results */
    sqlite3_result_null(ctx);
  } else {
    return SQLITE_ERROR;
  }

  return SQLITE_OK;
}

/*
** Return rowid
*/
static int diskannRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid) {
  diskann_cursor *pCur = (diskann_cursor *)pCursor;

  if (pCur->current >= pCur->num_results) {
    return SQLITE_ERROR;
  }

  *pRowid = pCur->results[pCur->current].id;
  return SQLITE_OK;
}

/*
** INSERT/UPDATE/DELETE handler
**
** INSERT: argc==3, argv[0]=NULL, argv[1]=rowid (or NULL for auto),
** argv[2]=vector UPDATE: argc==3, argv[0]=old_rowid, argv[1]=new_rowid,
** argv[2]=vector DELETE: argc==2, argv[0]=rowid, argv[1]=NULL
*/
static int diskannUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
                         sqlite_int64 *pRowid) {
  diskann_vtab *p = (diskann_vtab *)pVtab;
  int rc;

  if (argc == 1) {
    /* DELETE: argv[0] = rowid */
    sqlite_int64 rowid = sqlite3_value_int64(argv[0]);
    rc = diskann_delete(p->idx, rowid);
    return (rc == DISKANN_OK) ? SQLITE_OK : SQLITE_ERROR;
  } else if (argc > 1 && sqlite3_value_type(argv[0]) == SQLITE_NULL) {
    /* INSERT: argv[0]=NULL, argv[1]=rowid or NULL, argv[2+]=data */
    sqlite_int64 rowid;
    const void *vector_blob;
    int blob_bytes;
    uint32_t dims;

    /* Get rowid */
    if (sqlite3_value_type(argv[1]) == SQLITE_NULL) {
      /* Auto-generate rowid - not supported yet */
      pVtab->zErrMsg =
          sqlite3_mprintf("diskann: auto-generated rowid not supported");
      return SQLITE_ERROR;
    }
    rowid = sqlite3_value_int64(argv[1]);

    /* Get vector */
    vector_blob = sqlite3_value_blob(argv[2]);
    blob_bytes = sqlite3_value_bytes(argv[2]);
    dims = (uint32_t)((size_t)blob_bytes / sizeof(float));

    if (!vector_blob || dims == 0) {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: invalid vector");
      return SQLITE_ERROR;
    }

    /* Insert */
    rc = diskann_insert(p->idx, rowid, (const float *)vector_blob, dims);
    if (rc == DISKANN_OK) {
      *pRowid = rowid;
      return SQLITE_OK;
    } else {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: insert failed");
      return SQLITE_ERROR;
    }
  } else {
    /* UPDATE - not supported */
    pVtab->zErrMsg = sqlite3_mprintf("diskann: UPDATE not supported");
    return SQLITE_ERROR;
  }
}

/*
** Virtual table module definition
*/
static sqlite3_module diskannModule = {
    0,                 /* iVersion */
    diskannConnect,    /* xCreate - same as xConnect for persistent tables */
    diskannConnect,    /* xConnect */
    diskannBestIndex,  /* xBestIndex */
    diskannDisconnect, /* xDisconnect */
    diskannDisconnect, /* xDestroy - same as xDisconnect */
    diskannOpen,       /* xOpen - open a cursor */
    diskannClose,      /* xClose - close a cursor */
    diskannFilter,     /* xFilter - configure a cursor */
    diskannNext,       /* xNext - advance a cursor */
    diskannEof,        /* xEof - check for end of scan */
    diskannColumn,     /* xColumn - read data */
    diskannRowid,      /* xRowid - read data */
    diskannUpdate,     /* xUpdate - write data */
    NULL,              /* xBegin */
    NULL,              /* xSync */
    NULL,              /* xCommit */
    NULL,              /* xRollback */
    NULL,              /* xFindFunction */
    NULL,              /* xRename */
    NULL,              /* xSavepoint */
    NULL,              /* xRelease */
    NULL,              /* xRollbackTo */
    NULL,              /* xShadowName */
    NULL,              /* xIntegrity */
};

/*
** Register the diskann virtual table module
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

  /* Success */
  return SQLITE_OK;
}
