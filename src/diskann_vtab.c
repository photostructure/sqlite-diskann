/*
** Virtual table implementation for DiskANN
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** Wraps the DiskANN C API as a SQLite virtual table.
** Supports CREATE, INSERT, SELECT (MATCH search), DELETE, DROP.
**
** Schema: CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN, meta1 TYPE,
*...)
** First 3 columns HIDDEN. Metadata columns visible in SELECT *.
** rowid via xRowid. MATCH on vector col for ANN search.
**
** Usage:
**   CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean, cat
*TEXT);
**   INSERT INTO t(rowid, vector, cat) VALUES (1, X'...', 'landscape');
**   SELECT rowid, distance, cat FROM t WHERE vector MATCH ?query AND k = 10;
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
#include "diskann_util.h"
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
#define DISKANN_IDX_FILTER 0x10

/* Maximum number of filter constraints in a single query */
#define DISKANN_MAX_FILTERS 16

/* Column indices in the vtab schema */
#define DISKANN_COL_VECTOR 0
#define DISKANN_COL_DISTANCE 1
#define DISKANN_COL_K 2
#define DISKANN_COL_META_START 3 /* First metadata column index */

/* Metadata column definition (parsed from CREATE VIRTUAL TABLE args) */
typedef struct DiskAnnMetaCol {
  char *name; /* sqlite3_mprintf'd, owned */
  char *type; /* sqlite3_mprintf'd, owned */
} DiskAnnMetaCol;

/* Virtual table structure */
typedef struct diskann_vtab {
  sqlite3_vtab base;
  sqlite3 *db;
  char *db_name;
  char *table_name;
  DiskAnnIndex *idx;   /* Opened index (kept open for performance) */
  uint32_t dimensions; /* Cached from idx for dim validation in xUpdate */
  int n_meta_cols;     /* 0 for vtabs without metadata columns */
  DiskAnnMetaCol
      *meta_cols; /* sqlite3_malloc'd array, NULL if n_meta_cols==0 */
} diskann_vtab;

/* Cursor structure for iteration */
typedef struct diskann_cursor {
  sqlite3_vtab_cursor base;
  DiskAnnResult *results;    /* Search results (sqlite3_malloc'd) */
  int num_results;           /* Actual count from diskann_search() */
  int current;               /* Current position (0-based) */
  sqlite3_stmt *meta_stmt;   /* Cached SELECT from _attrs, or NULL */
  int64_t meta_cached_rowid; /* Rowid of last fetched metadata row */
  int meta_has_row;          /* 1 if meta_stmt stepped to SQLITE_ROW */
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
** Free a DiskAnnMetaCol array and all owned strings.
*/
static void free_meta_cols(DiskAnnMetaCol *cols, int n) {
  if (!cols)
    return;
  for (int i = 0; i < n; i++) {
    sqlite3_free(cols[i].name);
    sqlite3_free(cols[i].type);
  }
  sqlite3_free(cols);
}

/*
** Check if a column name is reserved (case-insensitive).
*/
static int is_reserved_column_name(const char *name) {
  return sqlite3_stricmp(name, "vector") == 0 ||
         sqlite3_stricmp(name, "distance") == 0 ||
         sqlite3_stricmp(name, "k") == 0 || sqlite3_stricmp(name, "rowid") == 0;
}

/*
** Check if a metadata column type is valid (case-insensitive).
*/
static int is_valid_meta_type(const char *type) {
  return sqlite3_stricmp(type, "TEXT") == 0 ||
         sqlite3_stricmp(type, "INTEGER") == 0 ||
         sqlite3_stricmp(type, "REAL") == 0 ||
         sqlite3_stricmp(type, "BLOB") == 0;
}

/*
** Parse metadata column definitions from argv.
** Non-key=value entries are treated as "name TYPE" column definitions.
** Validates names, types, rejects duplicates and reserved names.
** On success, *out_cols and *out_n are set (caller owns *out_cols).
** On failure, *pzErr is set and SQLITE_ERROR returned.
*/
static int parse_meta_columns(const char *const *argv, int argc,
                              DiskAnnMetaCol **out_cols, int *out_n,
                              char **pzErr) {
  /* First pass: count non-key=value entries */
  int n = 0;
  for (int i = 3; i < argc; i++) {
    if (!strchr(argv[i], '='))
      n++;
  }
  if (n == 0) {
    *out_cols = NULL;
    *out_n = 0;
    return SQLITE_OK;
  }

  DiskAnnMetaCol *cols = sqlite3_malloc(n * (int)sizeof(DiskAnnMetaCol));
  if (!cols)
    return SQLITE_NOMEM;
  memset(cols, 0, (size_t)n * sizeof(DiskAnnMetaCol));

  int idx = 0;
  for (int i = 3; i < argc; i++) {
    if (strchr(argv[i], '='))
      continue;

    /* Parse "name TYPE" */
    char name_buf[MAX_IDENTIFIER_LEN + 1];
    char type_buf[16];
    if (sscanf(argv[i], "%64s %15s", name_buf, type_buf) != 2) {
      *pzErr =
          sqlite3_mprintf("diskann: invalid column definition '%s'", argv[i]);
      free_meta_cols(cols, idx);
      return SQLITE_ERROR;
    }

    /* Validate name */
    if (!validate_identifier(name_buf)) {
      *pzErr = sqlite3_mprintf("diskann: invalid column name '%s'", name_buf);
      free_meta_cols(cols, idx);
      return SQLITE_ERROR;
    }
    if (is_reserved_column_name(name_buf)) {
      *pzErr = sqlite3_mprintf("diskann: reserved column name '%s'", name_buf);
      free_meta_cols(cols, idx);
      return SQLITE_ERROR;
    }

    /* Check for duplicate column names */
    for (int j = 0; j < idx; j++) {
      if (sqlite3_stricmp(cols[j].name, name_buf) == 0) {
        *pzErr =
            sqlite3_mprintf("diskann: duplicate column name '%s'", name_buf);
        free_meta_cols(cols, idx);
        return SQLITE_ERROR;
      }
    }

    /* Validate type */
    if (!is_valid_meta_type(type_buf)) {
      *pzErr = sqlite3_mprintf("diskann: invalid column type '%s' for '%s' "
                               "(must be TEXT, INTEGER, REAL, or BLOB)",
                               type_buf, name_buf);
      free_meta_cols(cols, idx);
      return SQLITE_ERROR;
    }

    cols[idx].name = sqlite3_mprintf("%s", name_buf);
    cols[idx].type = sqlite3_mprintf("%s", type_buf);
    if (!cols[idx].name || !cols[idx].type) {
      free_meta_cols(cols, idx + 1);
      return SQLITE_NOMEM;
    }
    idx++;
  }

  *out_cols = cols;
  *out_n = idx;
  return SQLITE_OK;
}

/*
** Shared init helper for xCreate and xConnect.
** Declares the vtab schema (with optional metadata columns),
** allocates the vtab struct, populates fields.
** Takes ownership of meta_cols on success; caller must free on failure.
*/
static int vtab_init(sqlite3 *db, const char *db_name, const char *table_name,
                     DiskAnnIndex *idx, DiskAnnMetaCol *meta_cols,
                     int n_meta_cols, sqlite3_vtab **ppVtab, char **pzErr) {
  int rc;

  /* Build dynamic declare_vtab schema string */
  sqlite3_str *s = sqlite3_str_new(db);
  sqlite3_str_appendall(
      s, "CREATE TABLE x(vector HIDDEN, distance HIDDEN, k HIDDEN");
  for (int i = 0; i < n_meta_cols; i++) {
    sqlite3_str_appendf(s, ", \"%w\" %s", meta_cols[i].name, meta_cols[i].type);
  }
  sqlite3_str_appendall(s, ")");

  char *schema = sqlite3_str_finish(s);
  if (!schema) {
    *pzErr = sqlite3_mprintf("diskann: out of memory building schema");
    return SQLITE_NOMEM;
  }

  rc = sqlite3_declare_vtab(db, schema);
  sqlite3_free(schema);
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
  pVtab->n_meta_cols = n_meta_cols;
  pVtab->meta_cols = meta_cols; /* Takes ownership */

  if (!pVtab->db_name || !pVtab->table_name) {
    sqlite3_free(pVtab->db_name);
    sqlite3_free(pVtab->table_name);
    /* Don't free meta_cols here — caller still owns on failure */
    pVtab->meta_cols = NULL;
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
  DiskAnnMetaCol *meta_cols = NULL;
  int n_meta_cols = 0;
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

  /* Parse CREATE VIRTUAL TABLE key=value parameters */
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

  /* Parse metadata column definitions (non-key=value entries) */
  rc = parse_meta_columns(argv, argc, &meta_cols, &n_meta_cols, pzErr);
  if (rc != SQLITE_OK)
    return rc;

  /* Create index (shadow tables + metadata) */
  rc = diskann_create_index(db, db_name, table_name, &config);
  if (rc != DISKANN_OK && rc != DISKANN_ERROR_EXISTS) {
    *pzErr = sqlite3_mprintf("diskann: failed to create index (rc=%d)", rc);
    free_meta_cols(meta_cols, n_meta_cols);
    return SQLITE_ERROR;
  }

  /* Create Phase 2 shadow tables if we have metadata columns */
  if (n_meta_cols > 0) {
    char *sql = NULL;
    char *err_msg = NULL;

    /* Create _columns table: persists column definitions for xConnect */
    sql = sqlite3_mprintf("CREATE TABLE \"%w\".\"%w_columns\" ("
                          "name TEXT NOT NULL, type TEXT NOT NULL)",
                          db_name, table_name);
    if (!sql) {
      free_meta_cols(meta_cols, n_meta_cols);
      diskann_drop_index(db, db_name, table_name);
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
      *pzErr = sqlite3_mprintf("diskann: failed to create _columns table: %s",
                               err_msg ? err_msg : "unknown error");
      if (err_msg)
        sqlite3_free(err_msg);
      free_meta_cols(meta_cols, n_meta_cols);
      diskann_drop_index(db, db_name, table_name);
      return SQLITE_ERROR;
    }

    /* Insert column definitions into _columns */
    for (int i = 0; i < n_meta_cols; i++) {
      sql = sqlite3_mprintf(
          "INSERT INTO \"%w\".\"%w_columns\"(name, type) VALUES ('%q', '%q')",
          db_name, table_name, meta_cols[i].name, meta_cols[i].type);
      if (!sql) {
        free_meta_cols(meta_cols, n_meta_cols);
        diskann_drop_index(db, db_name, table_name);
        return SQLITE_NOMEM;
      }
      rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
      sqlite3_free(sql);
      if (rc != SQLITE_OK) {
        if (err_msg)
          sqlite3_free(err_msg);
        free_meta_cols(meta_cols, n_meta_cols);
        diskann_drop_index(db, db_name, table_name);
        return SQLITE_ERROR;
      }
    }

    /* Create _attrs table with dynamic schema */
    sqlite3_str *s = sqlite3_str_new(db);
    sqlite3_str_appendf(
        s, "CREATE TABLE \"%w\".\"%w_attrs\"(rowid INTEGER PRIMARY KEY",
        db_name, table_name);
    for (int i = 0; i < n_meta_cols; i++) {
      sqlite3_str_appendf(s, ", \"%w\" %s", meta_cols[i].name,
                          meta_cols[i].type);
    }
    sqlite3_str_appendall(s, ")");
    sql = sqlite3_str_finish(s);
    if (!sql) {
      free_meta_cols(meta_cols, n_meta_cols);
      diskann_drop_index(db, db_name, table_name);
      return SQLITE_NOMEM;
    }
    rc = sqlite3_exec(db, sql, NULL, NULL, &err_msg);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
      *pzErr = sqlite3_mprintf("diskann: failed to create _attrs table: %s",
                               err_msg ? err_msg : "unknown error");
      if (err_msg)
        sqlite3_free(err_msg);
      free_meta_cols(meta_cols, n_meta_cols);
      diskann_drop_index(db, db_name, table_name);
      return SQLITE_ERROR;
    }
  }

  /* Open the index */
  rc = diskann_open_index(db, db_name, table_name, &idx);
  if (rc != DISKANN_OK) {
    *pzErr = sqlite3_mprintf("diskann: failed to open index (rc=%d)", rc);
    free_meta_cols(meta_cols, n_meta_cols);
    diskann_drop_index(db, db_name, table_name);
    return SQLITE_ERROR;
  }

  rc = vtab_init(db, db_name, table_name, idx, meta_cols, n_meta_cols, ppVtab,
                 pzErr);
  if (rc != SQLITE_OK) {
    diskann_close_index(idx);
    free_meta_cols(meta_cols, n_meta_cols);
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
  DiskAnnMetaCol *meta_cols = NULL;
  int n_meta_cols = 0;
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

  /* Read metadata column definitions from _columns table (Phase 2).
  ** If _columns doesn't exist (Phase 1 index), n_meta_cols stays 0. */
  char *col_sql = sqlite3_mprintf(
      "SELECT name, type FROM \"%w\".\"%w_columns\"", db_name, table_name);
  if (!col_sql) {
    diskann_close_index(idx);
    return SQLITE_NOMEM;
  }

  sqlite3_stmt *col_stmt;
  rc = sqlite3_prepare_v2(db, col_sql, -1, &col_stmt, NULL);
  sqlite3_free(col_sql);

  if (rc == SQLITE_OK) {
    /* Count rows first */
    int count = 0;
    while (sqlite3_step(col_stmt) == SQLITE_ROW)
      count++;
    sqlite3_reset(col_stmt);

    if (count > 0) {
      meta_cols = sqlite3_malloc(count * (int)sizeof(DiskAnnMetaCol));
      if (!meta_cols) {
        sqlite3_finalize(col_stmt);
        diskann_close_index(idx);
        return SQLITE_NOMEM;
      }
      memset(meta_cols, 0, (size_t)count * sizeof(DiskAnnMetaCol));

      int i = 0;
      while (sqlite3_step(col_stmt) == SQLITE_ROW && i < count) {
        const char *name = (const char *)sqlite3_column_text(col_stmt, 0);
        const char *type = (const char *)sqlite3_column_text(col_stmt, 1);
        meta_cols[i].name = sqlite3_mprintf("%s", name);
        meta_cols[i].type = sqlite3_mprintf("%s", type);
        if (!meta_cols[i].name || !meta_cols[i].type) {
          free_meta_cols(meta_cols, i + 1);
          sqlite3_finalize(col_stmt);
          diskann_close_index(idx);
          return SQLITE_NOMEM;
        }
        i++;
      }
      n_meta_cols = i;
    }
    sqlite3_finalize(col_stmt);
  } else {
    /* _columns table doesn't exist — Phase 1 index, no metadata cols */
    n_meta_cols = 0;
  }

  rc = vtab_init(db, db_name, table_name, idx, meta_cols, n_meta_cols, ppVtab,
                 pzErr);
  if (rc != SQLITE_OK) {
    diskann_close_index(idx);
    free_meta_cols(meta_cols, n_meta_cols);
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
  free_meta_cols(p->meta_cols, p->n_meta_cols);
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

  /* Drop all shadow tables (including Phase 2 _attrs/_columns) */
  diskann_drop_index(p->db, p->db_name, p->table_name);

  free_meta_cols(p->meta_cols, p->n_meta_cols);
  sqlite3_free(p->db_name);
  sqlite3_free(p->table_name);
  sqlite3_free(p);
  return SQLITE_OK;
}

/**************************************************************************
** DiskAnnRowidSet — Pre-computed filter for beam search
**
** Built from SQL query on _attrs shadow table, used as DiskAnnFilterFn
** callback context. Binary search on sorted rowid array.
**************************************************************************/

typedef struct DiskAnnRowidSet {
  int64_t *rowids; /* Sorted ascending, sqlite3_malloc'd */
  int count;
} DiskAnnRowidSet;

/* Binary search callback — compatible with DiskAnnFilterFn signature */
static int rowid_set_contains(int64_t rowid, void *ctx) {
  DiskAnnRowidSet *set = (DiskAnnRowidSet *)ctx;
  int lo = 0, hi = set->count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (set->rowids[mid] == rowid)
      return 1;
    if (set->rowids[mid] < rowid)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

static void rowid_set_free(DiskAnnRowidSet *set) {
  if (set->rowids) {
    sqlite3_free(set->rowids);
    set->rowids = NULL;
  }
  set->count = 0;
}

/*
** Map SQLite constraint op to SQL operator string.
** Returns NULL for unsupported ops.
*/
static const char *constraint_op_to_sql(int op) {
  switch (op) {
  case SQLITE_INDEX_CONSTRAINT_EQ:
    return "=";
  case SQLITE_INDEX_CONSTRAINT_GT:
    return ">";
  case SQLITE_INDEX_CONSTRAINT_LE:
    return "<=";
  case SQLITE_INDEX_CONSTRAINT_LT:
    return "<";
  case SQLITE_INDEX_CONSTRAINT_GE:
    return ">=";
  case SQLITE_INDEX_CONSTRAINT_NE:
    return "!=";
  default:
    return NULL;
  }
}

/*
** xBestIndex — query planning.
** Recognizes MATCH (vector search), EQ on k, LIMIT, ROWID EQ,
** and metadata filter constraints (EQ/GT/LT/GE/LE/NE on columns >= 3).
*/
static int diskannBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  diskann_vtab *pVt = (diskann_vtab *)pVtab;
  int idxNum = 0;

  /* Pass 1: Find constraint positions.
  ** SQLite presents constraints in arbitrary order, but xFilter reads argv
  ** in a fixed order (MATCH, K, LIMIT, ROWID, then filters). We must assign
  ** argvIndex values that match xFilter's consumption order, not constraint
  ** array order. Record positions first, assign in pass 2. */
  int i_match = -1, i_k = -1, i_limit = -1, i_rowid = -1;

  /* Filter constraints on metadata columns */
  int n_filters = 0;
  int i_filter[DISKANN_MAX_FILTERS];   /* constraint array index */
  int filter_col[DISKANN_MAX_FILTERS]; /* col offset (iColumn - META_START) */
  int filter_op[DISKANN_MAX_FILTERS];  /* SQLite op value */

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
    } else if (c->iColumn >= DISKANN_COL_META_START &&
               c->iColumn < DISKANN_COL_META_START + pVt->n_meta_cols &&
               constraint_op_to_sql(c->op) != NULL &&
               n_filters < DISKANN_MAX_FILTERS) {
      /* Metadata filter constraint */
      i_filter[n_filters] = i;
      filter_col[n_filters] = c->iColumn - DISKANN_COL_META_START;
      filter_op[n_filters] = c->op;
      n_filters++;
    }
  }

  if (n_filters > 0) {
    idxNum |= DISKANN_IDX_FILTER;
  }

  /* Pass 2: Assign argvIndex in the order xFilter consumes them.
  ** Order: MATCH, K, LIMIT, ROWID, then filter constraints. */
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

  /* Assign argvIndex for filter constraints and build idxStr */
  if (n_filters > 0) {
    for (int i = 0; i < n_filters; i++) {
      pInfo->aConstraintUsage[i_filter[i]].argvIndex = next_argv++;
      pInfo->aConstraintUsage[i_filter[i]].omit = 0; /* SQLite double-checks */
    }

    /* Build idxStr: comma-separated "col_offset:op" pairs */
    sqlite3_str *s = sqlite3_str_new(NULL);
    for (int i = 0; i < n_filters; i++) {
      if (i > 0)
        sqlite3_str_appendall(s, ",");
      sqlite3_str_appendf(s, "%d:%d", filter_col[i], filter_op[i]);
    }
    pInfo->idxStr = sqlite3_str_finish(s);
    if (!pInfo->idxStr)
      return SQLITE_NOMEM;
    pInfo->needToFreeIdxStr = 1;
  }

  pInfo->idxNum = idxNum;

  if (idxNum & DISKANN_IDX_MATCH) {
    pInfo->estimatedCost = (n_filters > 0) ? 200.0 : 100.0;
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
  if (pCur->meta_stmt) {
    sqlite3_finalize(pCur->meta_stmt);
    pCur->meta_stmt = NULL;
  }
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

    int rc;
    if (idxNum & DISKANN_IDX_FILTER) {
      /* Build rowid set from metadata filter constraints */
      DiskAnnRowidSet rset = {NULL, 0};

      /* Parse idxStr: comma-separated "col_offset:op" pairs */
      int n_fc = 0;
      int fc_col[DISKANN_MAX_FILTERS];
      int fc_op[DISKANN_MAX_FILTERS];
      if (idxStr) {
        const char *p = idxStr;
        while (*p && n_fc < DISKANN_MAX_FILTERS) {
          fc_col[n_fc] = (int)strtol(p, (char **)&p, 10);
          if (*p == ':')
            p++;
          fc_op[n_fc] = (int)strtol(p, (char **)&p, 10);
          n_fc++;
          if (*p == ',')
            p++;
        }
      }

      /* Build SQL: SELECT rowid FROM _attrs WHERE col1 op1 ? AND ... */
      sqlite3_str *fs = sqlite3_str_new(pVtab->db);
      sqlite3_str_appendf(fs, "SELECT rowid FROM \"%w\".\"%w_attrs\" WHERE 1=1",
                          pVtab->db_name, pVtab->table_name);
      for (int fi = 0; fi < n_fc; fi++) {
        const char *op_str = constraint_op_to_sql(fc_op[fi]);
        if (op_str && fc_col[fi] >= 0 && fc_col[fi] < pVtab->n_meta_cols) {
          sqlite3_str_appendf(fs, " AND \"%w\" %s ?",
                              pVtab->meta_cols[fc_col[fi]].name, op_str);
        }
      }
      sqlite3_str_appendall(fs, " ORDER BY rowid");
      char *filter_sql = sqlite3_str_finish(fs);
      if (!filter_sql) {
        sqlite3_free(pCur->results);
        pCur->results = NULL;
        return SQLITE_NOMEM;
      }

      /* Execute filter query */
      sqlite3_stmt *fstmt;
      rc = sqlite3_prepare_v2(pVtab->db, filter_sql, -1, &fstmt, NULL);
      sqlite3_free(filter_sql);
      if (rc != SQLITE_OK) {
        sqlite3_free(pCur->results);
        pCur->results = NULL;
        return rc;
      }

      /* Bind filter values from argv */
      for (int fi = 0; fi < n_fc; fi++) {
        sqlite3_bind_value(fstmt, fi + 1, argv[next + fi]);
      }

      /* Collect rowids into dynamic array */
      int cap = 64;
      rset.rowids = sqlite3_malloc(cap * (int)sizeof(int64_t));
      if (!rset.rowids) {
        sqlite3_finalize(fstmt);
        sqlite3_free(pCur->results);
        pCur->results = NULL;
        return SQLITE_NOMEM;
      }
      rset.count = 0;

      while (sqlite3_step(fstmt) == SQLITE_ROW) {
        if (rset.count >= cap) {
          cap *= 2;
          int64_t *tmp =
              sqlite3_realloc(rset.rowids, cap * (int)sizeof(int64_t));
          if (!tmp) {
            sqlite3_finalize(fstmt);
            rowid_set_free(&rset);
            sqlite3_free(pCur->results);
            pCur->results = NULL;
            return SQLITE_NOMEM;
          }
          rset.rowids = tmp;
        }
        rset.rowids[rset.count++] = sqlite3_column_int64(fstmt, 0);
      }
      sqlite3_finalize(fstmt);

      /* Run filtered search */
      rc = diskann_search_filtered(pVtab->idx, query, query_dims, k,
                                   pCur->results, rowid_set_contains, &rset);
      rowid_set_free(&rset);
    } else {
      /* Unfiltered search */
      rc = diskann_search(pVtab->idx, query, query_dims, k, pCur->results);
    }

    if (rc < 0) {
      sqlite3_free(pCur->results);
      pCur->results = NULL;
      return SQLITE_ERROR;
    }
    pCur->num_results = rc; /* rc IS the count */
    pCur->current = 0;
    goto prepare_meta_stmt;
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
    goto prepare_meta_stmt;
  }

  /* No MATCH, no ROWID → empty result set */
  pCur->num_results = 0;
  return SQLITE_OK;

prepare_meta_stmt:
  /* Prepare metadata SELECT statement if vtab has metadata columns */
  if (pCur->meta_stmt) {
    sqlite3_finalize(pCur->meta_stmt);
    pCur->meta_stmt = NULL;
  }
  pCur->meta_cached_rowid = -1;
  pCur->meta_has_row = 0;

  if (pVtab->n_meta_cols > 0 && pCur->num_results > 0) {
    sqlite3_str *ms = sqlite3_str_new(pVtab->db);
    sqlite3_str_appendall(ms, "SELECT ");
    for (int mi = 0; mi < pVtab->n_meta_cols; mi++) {
      if (mi > 0)
        sqlite3_str_appendall(ms, ", ");
      sqlite3_str_appendf(ms, "\"%w\"", pVtab->meta_cols[mi].name);
    }
    sqlite3_str_appendf(ms, " FROM \"%w\".\"%w_attrs\" WHERE rowid = ?",
                        pVtab->db_name, pVtab->table_name);
    char *meta_sql = sqlite3_str_finish(ms);
    if (!meta_sql)
      return SQLITE_NOMEM;

    int mrc =
        sqlite3_prepare_v2(pVtab->db, meta_sql, -1, &pCur->meta_stmt, NULL);
    sqlite3_free(meta_sql);
    if (mrc != SQLITE_OK) {
      pCur->meta_stmt = NULL;
      /* Non-fatal: metadata fetch will return NULLs */
    }
  }
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
  default: {
    /* Metadata column: col >= DISKANN_COL_META_START */
    int meta_idx = i - DISKANN_COL_META_START;
    diskann_vtab *pVtab = (diskann_vtab *)pCursor->pVtab;
    if (meta_idx < 0 || meta_idx >= pVtab->n_meta_cols || !pCur->meta_stmt) {
      sqlite3_result_null(ctx);
      break;
    }

    /* Lazy fetch: only query _attrs when rowid changes */
    int64_t current_rowid = pCur->results[pCur->current].id;
    if (pCur->meta_cached_rowid != current_rowid) {
      sqlite3_reset(pCur->meta_stmt);
      sqlite3_bind_int64(pCur->meta_stmt, 1, current_rowid);
      pCur->meta_has_row = (sqlite3_step(pCur->meta_stmt) == SQLITE_ROW);
      pCur->meta_cached_rowid = current_rowid;
    }

    if (pCur->meta_has_row) {
      sqlite3_result_value(ctx,
                           sqlite3_column_value(pCur->meta_stmt, meta_idx));
    } else {
      sqlite3_result_null(ctx);
    }
    break;
  }
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
**         argv[4]=k(NULL), argv[5+i]=metadata[i]. argc = 2 + 3 + n_meta_cols.
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
    if (rc != DISKANN_OK && rc != DISKANN_ERROR_NOTFOUND) {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: delete failed (rc=%d)", rc);
      return SQLITE_ERROR;
    }

    /* Delete metadata row from _attrs (if metadata columns exist) */
    if (p->n_meta_cols > 0) {
      char *sql =
          sqlite3_mprintf("DELETE FROM \"%w\".\"%w_attrs\" WHERE rowid = ?",
                          p->db_name, p->table_name);
      if (sql) {
        sqlite3_stmt *del_stmt;
        if (sqlite3_prepare_v2(p->db, sql, -1, &del_stmt, NULL) == SQLITE_OK) {
          sqlite3_bind_int64(del_stmt, 1, rowid);
          sqlite3_step(del_stmt);
          sqlite3_finalize(del_stmt);
        }
        sqlite3_free(sql);
      }
    }
    return SQLITE_OK;
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

    /* argv[3]=distance(NULL), argv[4]=k(NULL) — skip
    ** argv[5+i] = metadata column i */
    int rc = diskann_insert(p->idx, rowid, vec, dims);
    if (rc != DISKANN_OK) {
      pVtab->zErrMsg = sqlite3_mprintf("diskann: insert failed (rc=%d)", rc);
      return SQLITE_ERROR;
    }

    /* Insert metadata row into _attrs (if metadata columns exist) */
    if (p->n_meta_cols > 0) {
      sqlite3_str *s = sqlite3_str_new(p->db);
      sqlite3_str_appendf(s, "INSERT INTO \"%w\".\"%w_attrs\"(rowid",
                          p->db_name, p->table_name);
      for (int mi = 0; mi < p->n_meta_cols; mi++) {
        sqlite3_str_appendf(s, ", \"%w\"", p->meta_cols[mi].name);
      }
      sqlite3_str_appendall(s, ") VALUES (?");
      for (int mi = 0; mi < p->n_meta_cols; mi++) {
        sqlite3_str_appendall(s, ", ?");
      }
      sqlite3_str_appendall(s, ")");
      char *sql = sqlite3_str_finish(s);
      if (!sql) {
        return SQLITE_NOMEM;
      }

      sqlite3_stmt *ins_stmt;
      rc = sqlite3_prepare_v2(p->db, sql, -1, &ins_stmt, NULL);
      sqlite3_free(sql);
      if (rc != SQLITE_OK) {
        pVtab->zErrMsg =
            sqlite3_mprintf("diskann: failed to prepare _attrs insert");
        return SQLITE_ERROR;
      }

      sqlite3_bind_int64(ins_stmt, 1, rowid);
      for (int mi = 0; mi < p->n_meta_cols; mi++) {
        /* argv[5+mi] = metadata column mi (after rowid, vector, distance, k) */
        sqlite3_bind_value(ins_stmt, 2 + mi, argv[5 + mi]);
      }

      rc = sqlite3_step(ins_stmt);
      sqlite3_finalize(ins_stmt);
      if (rc != SQLITE_DONE) {
        pVtab->zErrMsg = sqlite3_mprintf("diskann: failed to insert metadata");
        return SQLITE_ERROR;
      }
    }

    *pRowid = rowid;
    return SQLITE_OK;
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
         sqlite3_stricmp(zName, "metadata") == 0 ||
         sqlite3_stricmp(zName, "attrs") == 0 ||
         sqlite3_stricmp(zName, "columns") == 0;
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
