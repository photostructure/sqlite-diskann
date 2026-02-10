/*
** DiskANN Search — k-NN beam search and search context
**
** Derived from libSQL DiskANN implementation
** Original Copyright 2024 the libSQL authors
** Modifications Copyright 2025 PhotoStructure Inc.
** MIT License
**
** This module provides:
** - DiskAnnSearchCtx — context for beam search traversal
** - diskann_search_internal() — core beam search (shared by search & insert)
** - diskann_select_random_shadow_row() — random start node selection
** - diskann_search() — public k-NN search API
*/
#ifndef DISKANN_SEARCH_H
#define DISKANN_SEARCH_H

#include "diskann_blob.h"
#include "diskann_internal.h"
#include "diskann_node.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Search context — manages candidates, visited nodes, and top-K results
** during beam search traversal.
**
** Memory ownership:
** - query: borrowed pointer (NOT owned, NOT freed)
** - candidates / distances: owned parallel arrays (malloc'd)
** - top_candidates / top_distances: owned parallel arrays (malloc'd)
** - visited_list: linked list of visited DiskAnnNodes (all freed in deinit)
** - Unvisited candidates in the candidates array are also freed in deinit
*/
typedef struct DiskAnnSearchCtx {
  const float *query;              /* borrowed, not owned */
  DiskAnnNode **candidates;        /* sorted by distance ascending */
  float *distances;                /* parallel to candidates */
  int n_candidates;
  int max_candidates;              /* = searchL or insertL */
  DiskAnnNode **top_candidates;    /* top-K exact results */
  float *top_distances;
  int n_top_candidates;
  int max_top_candidates;          /* = k */
  DiskAnnNode *visited_list;       /* linked list of visited nodes */
  int n_unvisited;
  int blob_mode;                   /* DISKANN_BLOB_READONLY or WRITABLE */
} DiskAnnSearchCtx;

/*
** Initialize search context. Allocates candidate and top-K arrays.
**
** Parameters:
**   ctx             - context to initialize (must not be NULL)
**   query           - query vector (borrowed, must outlive ctx)
**   max_candidates  - beam width (searchL for search, insertL for insert)
**   max_top         - number of top results to track (k)
**   blob_mode       - DISKANN_BLOB_READONLY or DISKANN_BLOB_WRITABLE
**
** Returns DISKANN_OK on success, DISKANN_ERROR_NOMEM on allocation failure.
*/
int diskann_search_ctx_init(DiskAnnSearchCtx *ctx, const float *query,
                            int max_candidates, int max_top, int blob_mode);

/*
** Free all resources owned by the search context.
** Frees all nodes (visited + unvisited candidates) and arrays.
*/
void diskann_search_ctx_deinit(DiskAnnSearchCtx *ctx);

/*
** Select a random row from the shadow table as search entry point.
**
** Returns:
**   DISKANN_OK with *rowid set if a row was found
**   SQLITE_DONE (101) if the table is empty (not an error)
**   Negative error code on failure
*/
int diskann_select_random_shadow_row(const DiskAnnIndex *idx, uint64_t *rowid);

/*
** Core beam search algorithm. Traverses the DiskANN graph starting from
** start_rowid, populating ctx with candidates and top-K results.
**
** Used by both search (READONLY mode) and insert (WRITABLE mode).
**
** Returns DISKANN_OK on success, negative error code on failure.
*/
int diskann_search_internal(DiskAnnIndex *idx, DiskAnnSearchCtx *ctx,
                            uint64_t start_rowid);

#ifdef __cplusplus
}
#endif

#endif /* DISKANN_SEARCH_H */
