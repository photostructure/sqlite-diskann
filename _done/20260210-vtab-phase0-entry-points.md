# Vtab Phase 0: Consolidate Entry Points (DESIGN REVISED)

## Summary

**DESIGN REVISED:** Original plan to remove `diskann_sqlite.h` and split entry point won't work due to multi-file extension constraint. New minimal approach: Extract `validate_identifier()` to shared header, delete orphaned `diskann_extension.c`, add documentation. Keep `diskann_sqlite.h` as-is (it's correct and necessary). Infrastructure-only — no new tests, but existing 126 tests must still pass.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design (REVISED - original design was flawed)
- [x] Test-First Development (SKIP - no new tests for infrastructure)
- [x] Implementation
- [x] Integration
- [x] Cleanup & Documentation
- [x] Final Review

**PHASE COMPLETE** ✅

## Required Reading

- `_todo/20260210-extension-loading-fix.md` — **CRITICAL:** Multi-file extension problem
- `src/diskann_sqlite.h` — Current solution (works correctly, must keep)
- `src/diskann_vtab.c` — Current vtab with DISKANN_VTAB_MAIN
- `src/diskann_extension.c` — Orphaned file (not in SOURCES, can delete)
- `src/diskann_api.c:38` — `validate_identifier()` (static, needs extraction)
- Parent TPP: `20260210-virtual-table-with-filtering.md`

## Description

**Original Problem (Misunderstood):** Intern thought two files compete for entry point and INIT macros cause test segfaults.

**Actual Situation Discovered During Research:**

Current architecture is **correct by necessity**. We have a **multi-file extension** (6 .c files all calling SQLite functions: `diskann_api.c`, `diskann_blob.c`, `diskann_insert.c`, `diskann_node.c`, `diskann_search.c`, `diskann_vtab.c`).

When building as extension, ALL SQLite calls across ALL files must be routed through the `sqlite3_api` function pointer table. This requires:

1. ONE file has `SQLITE_EXTENSION_INIT1` (creates pointer table definition)
2. All other files have `extern const sqlite3_api_routines *sqlite3_api`
3. All files include `<sqlite3ext.h>` for routing macros

The `diskann_sqlite.h` header **correctly solves this problem**:

- Extension builds: routes SQLite calls through function pointers
- Test builds: includes `<sqlite3.h>` directly for native linking
- One file (vtab) owns definition via `DISKANN_VTAB_MAIN`, others use `extern`

**Why Original Plan Won't Work:**

Original plan proposed having `diskann_vtab.c` include `<sqlite3.h>` directly. This breaks extension builds because vtab calls SQLite functions (`sqlite3_create_module_v2`, `sqlite3_malloc`, `sqlite3_declare_vtab`, etc.) which would become direct function calls → "undefined symbol" errors.

**Same problem applies to all 6 .c files.** They all need API routing. You cannot have a multi-file extension without the header shenanigans.

**Real Problems to Fix:**

1. `diskann_extension.c` exists but is orphaned (not in SOURCES, never compiled)
2. `validate_identifier()` should be shared across files to prevent duplication

**Success criteria:** Extract utility function, delete orphaned file, optionally add documentation. All 126 tests pass. Extension still loads. No architectural changes.

## Implementation Design (REVISED)

### Why Original Plan Won't Work

From `_todo/20260210-extension-loading-fix.md`:

> Our extension links in ALL the DiskANN source files... These files call SQLite functions DIRECTLY, not through extension API routing. When building as an extension (not linking `-lsqlite3`), those symbols are undefined.
>
> **sqlite-vec doesn't have this problem** because it's a single-file amalgamation where ALL code is inside the extension and has access to the routed API pointers.

We are NOT a single-file amalgamation. We NEED the conditional compilation in `diskann_sqlite.h`.

### Revised Approach: Minimal Useful Changes

**Change 1: Extract validate_identifier() to shared header**

**Create:** `src/diskann_util.h`

```c
/*
** Shared utility functions for DiskANN
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#ifndef DISKANN_UTIL_H
#define DISKANN_UTIL_H

#define MAX_IDENTIFIER_LEN 64

/*
** Validate a SQL identifier (index name or database name).
** Must match [a-zA-Z_][a-zA-Z0-9_]*, max MAX_IDENTIFIER_LEN chars.
** Returns 1 if valid, 0 if invalid.
**
** SECURITY: Prevents SQL injection by validating all identifiers
** before use in dynamic SQL. TypeScript layer also validates.
*/
static inline int validate_identifier(const char *name) {
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

#endif /* DISKANN_UTIL_H */
```

**Modify:** `src/diskann_api.c`

- Remove lines 31-56 (local `MAX_IDENTIFIER_LEN` and `validate_identifier()` definitions)
- Add `#include "diskann_util.h"` after other includes

**Change 2: Delete orphaned file**

**Delete:** `src/diskann_extension.c` — not in SOURCES, never compiled, superseded by diskann_vtab.c

**Change 3: Add documentation (optional but recommended)**

**Modify:** `src/diskann_sqlite.h` — replace existing header comment with:

```c
/*
** SQLite API access for DiskANN extension
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** MULTI-FILE EXTENSION PROBLEM:
** ==============================
** DiskANN is a multi-file extension (6 .c files all calling SQLite functions).
** When building as extension, ALL SQLite calls must be routed through the
** sqlite3_api function pointer table. This header solves the problem:
**
** Extension builds (-DDISKANN_EXTENSION):
**   - Includes <sqlite3ext.h> which provides routing macros
**   - One file (diskann_vtab.c) defines DISKANN_VTAB_MAIN and owns the
**     sqlite3_api definition via SQLITE_EXTENSION_INIT1
**   - Other files get extern declaration to access the same pointer table
**
** Test builds (no flag):
**   - Includes <sqlite3.h> directly for native linking with vendored SQLite
**   - No extension macros needed
**
** This is NOT optional - removing it will cause "undefined symbol" errors.
** See _todo/20260210-extension-loading-fix.md for full explanation.
*/
```

**Modify:** `src/diskann_vtab.c` — add comment before `#define DISKANN_VTAB_MAIN`:

```c
/* Mark this as the main file that defines sqlite3_api (not extern).
** Other .c files that include diskann_sqlite.h will get extern declaration.
** This is required for multi-file extensions - do not remove. */
#define DISKANN_VTAB_MAIN
```

### What We're NOT Doing

- **NOT removing diskann_sqlite.h** — Required for multi-file extension API routing
- **NOT creating diskann_ext.c** — Current single-file entry point is simpler
- **NOT creating diskann_vtab.h** — Not needed (tests don't call module registration directly)
- **NOT modifying vtab.c includes** — Must keep diskann_sqlite.h for extension builds
- **NOT modifying Makefile** — No changes needed (diskann_extension.c already not in SOURCES)

## Tasks (REVISED)

- [x] Create `src/diskann_util.h` with `validate_identifier()` (copy full implementation from diskann_api.c:38-56)
- [x] Update `src/diskann_api.c` — remove lines 31-56, add `#include "diskann_util.h"`
- [x] Delete `src/diskann_extension.c` (orphaned file, safe to remove)
- [x] Add documentation to `src/diskann_sqlite.h` header comment (optional but recommended)
- [x] Add comment to `src/diskann_vtab.c` before `DISKANN_VTAB_MAIN` (optional but recommended)
- [x] Verify: `make clean && make test` (should pass: 126 Tests 0 Failures 0 Ignored - OK)
- [x] Verify: `make clean && make all` (should build: diskann.so)
- [x] Verify: Extension loads: `sqlite3 :memory: ".load ./build/diskann.so" "CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=euclidean);" "SELECT 'Success';"`
- [x] Verify: `make asan` (should pass with no memory errors)

## Notes

**Why Design Was Revised:**

After researching `_todo/20260210-extension-loading-fix.md`, discovered the multi-file extension problem. The original plan to remove `diskann_sqlite.h` and have files include `<sqlite3.h>` directly won't work:

1. **All 6 .c files** call SQLite functions, not just vtab
2. **Extension builds** require ALL calls routed through `sqlite3_api` pointer table
3. **Removing conditional includes** causes "undefined symbol" errors when loading extension
4. **Current architecture is correct** by necessity, not by choice

**What Changed from Original Design:**

| Original Plan                 | Why It Won't Work                       | Revised Approach      |
| ----------------------------- | --------------------------------------- | --------------------- |
| Remove diskann_sqlite.h       | Breaks multi-file extension API routing | Keep it (required)    |
| Create diskann_ext.c          | Adds complexity without benefit         | Don't create          |
| Create diskann_vtab.h         | Not needed by tests                     | Don't create          |
| Have vtab include <sqlite3.h> | Causes undefined symbols in extension   | Keep diskann_sqlite.h |
| Modify Makefile               | Not needed                              | No changes            |

**What We're Still Doing:**

- ✅ Extract `validate_identifier()` to shared header (good idea, prevents duplication)
- ✅ Delete orphaned `diskann_extension.c` (safe cleanup)
- ✅ Add documentation explaining architecture (helps future maintainers)

**Scope Reduction:**

This phase is now much simpler — just utility extraction, file deletion, and optional docs. The existing entry point architecture stays as-is because it's correct. Phase 1 will rewrite the vtab methods, but the extension loading mechanism doesn't need changes.
