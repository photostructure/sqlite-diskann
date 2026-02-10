# Vtab Phase 3: Filtered Search

## Summary

Inject a filter callback into the DiskANN beam search so metadata constraints are evaluated **during** graph traversal (Filtered-DiskANN algorithm). Non-matching nodes are still traversed as graph bridges but excluded from top-K results. 16 new tests (11 vtab SQL + 5 C API unit).

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- Parent TPP: `20260210-virtual-table-with-filtering.md` — Filter gate design, beam width
- `src/diskann_search.c:58-84` — `search_ctx_mark_visited()` — THE filter injection point
- `src/diskann_search.h` — `DiskAnnSearchCtx` struct
- `src/diskann.h` — Public API (will add `diskann_search_filtered`)
- `src/diskann_insert.c` — Caller of `search_ctx_init` (must remain unaffected)
- Filtered-DiskANN paper: https://harsha-simhadri.org/pubs/Filtered-DiskANN23.pdf
- Blocked by: `20260210-vtab-phase2-metadata.md`

## Description

**Problem:** Phase 2 vtab has metadata columns but no way to filter search results by them. Post-filtering wastes results. Need in-traversal filtering per Filtered-DiskANN paper.

**Success criteria:** 16 new tests pass. Filtered search returns only matching results. Recall@10 >= 70% with 50% selectivity. Graph bridge traversal works (non-matching nodes still reachable).

## Implementation Design

### Core: Filter Callback Type

In `diskann_search.h`:

```c
/* Returns 1 to accept rowid in top-K results, 0 to reject.
** Rejected nodes are still visited for graph traversal. */
typedef int (*DiskAnnFilterFn)(int64_t rowid, void *ctx);
```

### DiskAnnSearchCtx Changes

Add two fields (both default to NULL in `search_ctx_init`):

```c
DiskAnnFilterFn filter_fn;   /* NULL = no filter */
void *filter_ctx;            /* Opaque context for filter callback */
```

No signature change to `diskann_search_ctx_init()` — just set fields to NULL. Callers (`diskann_search`, `diskann_insert`) are unaffected.

### Filter Gate in search_ctx_mark_visited

This is the critical change. In `diskann_search.c:58-84`:

```c
static void search_ctx_mark_visited(DiskAnnSearchCtx *ctx, DiskAnnNode *node,
                                    float distance) {
  /* ALWAYS: graph traversal bookkeeping (unchanged) */
  node->visited = 1;
  ctx->n_unvisited--;
  node->next = ctx->visited_list;
  ctx->visited_list = node;

  /* FILTER GATE: skip top-K insertion if filter rejects this rowid */
  if (ctx->filter_fn &&
      !ctx->filter_fn((int64_t)node->rowid, ctx->filter_ctx)) {
    return;
  }

  /* CONDITIONALLY: add to top-K results (existing code, unchanged) */
  int insert_idx = distance_buffer_insert_idx(ctx->top_distances,
      ctx->n_top_candidates, ctx->max_top_candidates, distance);
  if (insert_idx < 0) return;
  /* ... buffer_insert calls unchanged ... */
}
```

**Why this placement:** Filtered-DiskANN paper + Microsoft Rust confirm: non-matching nodes must be visited (graph bridges to matching nodes elsewhere). Only the result set is filtered, not the traversal.

### New Public API

In `diskann.h`:

```c
int diskann_search_filtered(DiskAnnIndex *idx, const float *query, uint32_t dims,
                            int k, DiskAnnResult *results,
                            DiskAnnFilterFn filter_fn, void *filter_ctx);
```

Implementation in `diskann_search.c` — same as `diskann_search()` except:

1. **Beam width:** `max(search_list_size * 2, (uint32_t)k * 4)` — wider beam compensates for filtered-out candidates
2. **Filter setup:** `ctx.filter_fn = filter_fn; ctx.filter_ctx = filter_ctx;` before calling `diskann_search_internal()`

### DiskAnnRowidSet (Pre-computed Filter)

```c
typedef struct DiskAnnRowidSet {
  int64_t *rowids;  /* Sorted ascending, sqlite3_malloc'd */
  int count;
} DiskAnnRowidSet;

/* Binary search callback for DiskAnnFilterFn */
static int rowid_set_contains(int64_t rowid, void *ctx);

/* Build from SQL query results */
static int rowid_set_build(sqlite3 *db, const char *sql,
                           sqlite3_value **argv, int argc,
                           DiskAnnRowidSet *out);
static void rowid_set_free(DiskAnnRowidSet *set);
```

### xBestIndex Changes

Detect metadata constraints (columns >= 3):

- Supported ops: EQ(2), GT(4), LE(8), LT(16), GE(32), NE(68)
- Set `idxNum |= 0x08` (FILTER bit)
- Assign argvIndex for each filter constraint (after MATCH, K, LIMIT)
- Build `idxStr` with `sqlite3_mprintf()`: comma-separated `"col_offset:op"` pairs
  - col_offset = `iColumn - 3`
  - op = SQLite constraint op value
  - Example: `"0:2,1:4"` = meta_col 0 EQ, meta_col 1 GT
- Set `needToFreeIdxStr = 1`
- Set `omit = 0` — let SQLite double-check filter results (safety)

### xFilter Changes

When `idxNum & 0x08`:

1. Parse idxStr to get `(col_offset, op)` pairs
2. Build SQL: `SELECT rowid FROM {name}_attrs WHERE {col} {op} ? AND ...`
   - Map op values to SQL operators: 2→"=", 4→">", 8→"<=", 16→"<", 32→">=", 68→"!="
   - Column names from `pVtab->meta_cols[col_offset].name`
   - Bind filter argv values as parameters (safe from injection)
3. Execute query, collect results into `DiskAnnRowidSet`
4. Call `diskann_search_filtered()` with `rowid_set_contains` callback
5. Free rowid set immediately after search returns

Without FILTER bit: call `diskann_search()` as before (Phase 1 path).

## Test Design

### C API Filter Tests (5) — in `tests/c/test_vtab.c`

44. `test_search_filtered_null_filter` — `diskann_search_filtered()` with NULL filter = same as `diskann_search()`
45. `test_search_filtered_accept_all` — filter returns 1 for everything = same as unfiltered
46. `test_search_filtered_reject_all` — filter returns 0 for everything = 0 results
47. `test_search_filtered_odd_only` — filter accepts odd rowids only. All results have odd IDs.
48. `test_search_filtered_validation` — NULL index/query/results, bad dims all return errors

### SQL Filter Tests (11) — in `tests/c/test_vtab.c`

Test data: 20 vectors, 3D euclidean. IDs 1-10: category='A', score=i*0.1. IDs 11-20: category='B', score=i*0.1+1.0.

**Equality (3):** 33. `test_vtab_filter_eq` — `category = 'A'` → only A rows returned 34. `test_vtab_filter_eq_other` — `category = 'B'` → only B rows 41. `test_vtab_filter_ne` — `category != 'A'` → only B rows

**Range (3):** 35. `test_vtab_filter_gt` — `score > 1.0` → only IDs 11-20 36. `test_vtab_filter_lt` — `score < 0.5` → only IDs 1-4 37. `test_vtab_filter_between` — `score >= 0.5 AND score <= 1.5` → IDs 5-15

**Combined (1):** 38. `test_vtab_filter_multi` — `category = 'A' AND score > 0.5` → IDs 6-10

**Edge cases (2):** 39. `test_vtab_filter_no_match` — `category = 'C'` → 0 rows 40. `test_vtab_filter_all_match` — `score > 0.0` → same as unfiltered

**Quality (2):** 42. `test_vtab_filter_recall` — 100 vectors (128D), 50/50 split. Recall@10 >= 70%. 43. `test_vtab_filter_graph_bridge` — Construct scenario: one 'A' node near query, reachable only through 'B' nodes. Verify the near 'A' node is found. (Core Filtered-DiskANN property.)

## Tasks

- [ ] Add `DiskAnnFilterFn` typedef to `diskann_search.h`
- [ ] Add filter fields to `DiskAnnSearchCtx` struct
- [ ] Set defaults (NULL) in `diskann_search_ctx_init()`
- [ ] Modify `search_ctx_mark_visited()` with filter gate
- [ ] Add `diskann_search_filtered()` to `diskann.h` + `diskann_search.c`
- [ ] Write 5 C API filter unit tests (failing)
- [ ] Make C API tests pass
- [ ] Implement `DiskAnnRowidSet` (sorted array + binary search)
- [ ] Implement xBestIndex metadata constraint detection + idxStr encoding
- [ ] Implement xFilter SQL generation + rowid set construction
- [ ] Write 11 SQL filter tests (failing)
- [ ] Make SQL filter tests pass
- [ ] All 48 tests pass (19 + 13 + 16)
- [ ] `make asan` clean
- [ ] `make clean && make valgrind` clean

## Notes

**Beam width heuristic may need tuning.** `max(search_list * 2, k * 4)` is a starting point. If `test_vtab_filter_recall` fails at 70% threshold, try `max(search_list * 3, k * 8)`.

**graph bridge test is the hardest to construct.** Need a vector geometry where the nearest 'A' node to the query is only reachable through 'B' nodes in the DiskANN graph. One approach: insert B cluster near query first (so graph connects through them), then insert distant A cluster, then insert one A node near query. The graph path from the random start to the near-A node goes through B nodes.

**Existing callers unaffected.** `diskann_search()` and `diskann_insert()` both call `diskann_search_ctx_init()` which sets `filter_fn = NULL`. No changes needed to those code paths.
