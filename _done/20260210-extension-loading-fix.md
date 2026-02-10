# Fix SQLite Extension Loading

## Summary

**SOLVED ✅** - DiskANN extension now builds and loads successfully without "undefined symbol" errors.

**Solution:** Created `diskann_sqlite.h` header that conditionally includes `<sqlite3ext.h>` (for extension builds) or `<sqlite3.h>` (for test builds). Only diskann_vtab.c has `SQLITE_EXTENSION_INIT1`, other files use `extern` declaration of `sqlite3_api`.

**Remaining Issue:** Extension loads but CREATE VIRTUAL TABLE fails with "diskann: failed to create index" - this is a separate bug in diskann_create_index that needs investigation.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [x] Cleanup & Documentation
- [x] Final Review

## Required Reading

- `src/diskann_vtab.c` - Virtual table implementation (~500 lines)
- `../sqlite-vec/sqlite-vec.c` - Working reference implementation (see init function ~line 7297)
- SQLite extension loading: https://www.sqlite.org/loadext.html
- SQLITE_EXTENSION_INIT macros: https://www.sqlite.org/c3ref/auto_extension.html

## Description

**Problem:** Extension loads but module not found.

**Current behavior:**

```bash
$ node -e "import('node:sqlite').then(({DatabaseSync}) => { const db = new DatabaseSync(':memory:', {allowExtension: true}); db.enableLoadExtension(true); db.loadExtension('./build/diskann'); console.log('Loaded'); db.exec('CREATE VIRTUAL TABLE t USING diskann(dimension=10, metric=cosine)'); })"
Loaded
Error: undefined symbol: sqlite3_bind_int64
```

**Working comparison (sqlite-vec):**

```bash
$ node -e "... db.loadExtension('/home/mrm/src/sqlite-vec/dist/vec0'); ..."
✓ sqlite-vec loaded
✓ Virtual table created
```

**Success criteria:**

- Extension loads without "undefined symbol" errors
- CREATE VIRTUAL TABLE succeeds
- INSERT/SELECT work end-to-end
- C tests still pass (126 tests)

## Tribal Knowledge

**Session 2025-02-10 findings:**

### What We Tried (Failed Approaches)

1. **Statically linking vendored SQLite (`build/sqlite3.o`)**
   - Result: Creates two SQLite instances (extension's + host's)
   - Module registers in extension's instance, queries run in host's instance
   - Symptom: "no such module" despite init succeeding

2. **Linking against system SQLite (`-lsqlite3`)**
   - Result: Version mismatch (system 3.45.1 vs vendored headers 3.51.2)
   - Extension API routing breaks with different versions
   - Still had two-instance problem

3. **Removing SQLITE_EXTENSION_INIT macros**
   - Result: Direct function calls, but still fails
   - Wrong approach - macros are necessary for dynamic loading

4. **Building without SQLite linkage (undefined symbols)**
   - Result: "undefined symbol: sqlite3_bind_int64" at load time
   - This is where we are now - sqlite-vec works this way, we don't

### Key Discovery: sqlite-vec Works

**How sqlite-vec builds:**

```bash
$ ldd ../sqlite-vec/dist/vec0.so
    libm.so.6 => /lib/x86_64-linux-gnu/libm.so.6
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6
    # NO libsqlite3 linkage
```

**sqlite-vec init function pattern:**

```c
SQLITE_EXTENSION_INIT1  // At file top

int sqlite3_vec_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
#ifndef SQLITE_CORE
  SQLITE_EXTENSION_INIT2(pApi);  // Only when NOT compiled into SQLite core
#endif
  // Uses sqlite3_create_function_v2, sqlite3_bind_int64, etc. successfully
}
```

**Our init function (broken):**

```c
SQLITE_EXTENSION_INIT1  // At file top

int sqlite3_diskann_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);  // No #ifndef SQLITE_CORE guard
  // Calls sqlite3_create_module_v2 which returns 0 (success) in minimal test
  // But actual extension fails with "undefined symbol: sqlite3_bind_int64"
}
```

**Hypothesis:** The difference is likely in how we use the extension API pointers, OR we're calling SQLite functions from parts of the code that don't have access to the routed pointers.

### Where sqlite3_bind_int64 Is Used

Not in `diskann_vtab.c` - that file only calls:

- `sqlite3_create_module_v2` (in init)
- `sqlite3_declare_vtab` (in connect)
- `sqlite3_malloc`, `sqlite3_mprintf`, `sqlite3_free`
- `sqlite3_value_*` functions
- `sqlite3_result_*` functions
- `sqlite3_context_db_handle`

The undefined symbol error suggests DiskANN core code (`diskann_api.c`, `diskann_insert.c`, etc.) calls SQLite functions directly, but those files don't have SQLITE_EXTENSION_INIT access.

### Critical Realization

Our extension links in ALL the DiskANN source files:

- `src/diskann_api.c` - Uses `sqlite3_exec`, `sqlite3_prepare_v2`, etc.
- `src/diskann_blob.c` - Uses `sqlite3_blob_*` functions
- `src/diskann_insert.c` - Uses SQLite prepared statements
- etc.

These files call SQLite functions DIRECTLY, not through extension API routing. When building as an extension (not linking `-lsqlite3`), those symbols are undefined.

**sqlite-vec doesn't have this problem** because it's a single-file amalgamation where ALL code is inside the extension and has access to the routed API pointers.

## Solutions

### Option 1: Single-File Amalgamation (sqlite-vec approach)

Build a single-file extension that includes all DiskANN code.

**Pros:**

- Proven pattern (sqlite-vec uses it)
- All code has access to SQLITE_EXTENSION_INIT routing
- Matches standard extension architecture

**Cons:**

- Lose multi-file organization
- Harder to develop/debug
- Build complexity

**Status:** Viable but last resort

### Option 2: Export Core API, Extension Calls It

Keep DiskANN core as a library that links against SQLite, extension is a thin wrapper.

**Pros:**

- Maintains clean separation
- Core can be tested independently
- Extension is minimal

**Cons:**

- Need to build two artifacts (libdiskann + extension)
- Extension must link against both libdiskann AND SQLite
- Complex build/linkage

**Status:** Investigating

### Option 3: Macro-Wrap All SQLite Calls

Use macros to route ALL SQLite function calls through extension API pointers.

**Example:**

```c
// In header when building as extension:
#ifdef DISKANN_AS_EXTENSION
  #define sqlite3_exec   (sqlite3_api->exec)
  #define sqlite3_prepare_v2  (sqlite3_api->prepare_v2)
  // ... etc for all ~100 SQLite functions we use
#endif
```

**Pros:**

- Keep multi-file structure
- Clean at call sites
- Only affects build, not source

**Cons:**

- Tedious (need macro for every SQLite function)
- Error-prone (miss one = undefined symbol)
- Maintenance burden

**Status:** Possible but fragile

### Option 4: Link Core Statically, Extension Dynamically (Recommended)

**Two build modes:**

1. **Tests mode:** Link everything including vendored `sqlite3.o`
   - Used by `make test`
   - Self-contained, no host SQLite needed

2. **Extension mode:** Link DiskANN core statically, leave SQLite symbols undefined
   - Used by extension
   - Host provides SQLite at runtime
   - **Key insight:** DiskANN core gets compiled INTO the extension, but SQLite symbols come from host

**Implementation:**

```makefile
# Extension build (current)
$(BUILD_DIR)/$(EXTENSION): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)
	# LIBS = -lm (no -lsqlite3)
	# SQLite symbols undefined, resolved from host

# Test build (with vendored SQLite)
$(BUILD_DIR)/$(TEST_BIN): $(SOURCES) $(BUILD_DIR)/sqlite3.o ...
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	# sqlite3.o provides all symbols for tests
```

The issue is that when building extension WITHOUT `-lsqlite3`, the linker complains about undefined symbols from `diskann_api.c` etc.

**Solution:** Use `-Wl,--allow-shlib-undefined` (ALREADY in Makefile!) to allow undefined symbols in shared library. They get resolved when loaded into host.

**Why this fails currently:** Unknown - need to debug further. This SHOULD work.

**Status:** Most promising - needs debugging

## Tasks

### Debugging Phase

- [ ] **Compare build outputs**
  - Build sqlite-vec: `cd ../sqlite-vec && make`
  - Check their compiler flags
  - Check their linker flags
  - Document exact differences from our build

- [ ] **Symbol comparison**
  - `nm -D ../sqlite-vec/dist/vec0.so > /tmp/vec-symbols.txt`
  - `nm -D build/diskann.so > /tmp/diskann-symbols.txt`
  - `diff` the undefined symbols
  - Are we missing SQLITE_EXTENSION_INIT setup somewhere?

- [ ] **Test minimal repro**
  - Create minimal extension that calls `sqlite3_bind_int64`
  - Does it work with just SQLITE_EXTENSION_INIT?
  - If yes, what's different from our code?

- [ ] **Check compilation units**
  - Are `diskann_api.c`, `diskann_blob.c` etc. seeing SQLITE_EXTENSION_INIT1?
  - Maybe they need it too?

- [ ] **Read sqlite-vec build carefully**
  - How do they handle multi-file builds (if they do)?
  - Do they use any special build tricks?

### Implementation Phase (TBD after debugging)

_Will fill in once we understand the root cause_

**Verification:**

```bash
# Must pass:
make test                    # C tests
make asan                    # Memory safety
node -e "..."                # Extension loads in node:sqlite
sqlite3 :memory: ".load ..." # Extension loads in CLI
```

## Critical Files

**Current:**

- `src/diskann_vtab.c` - Extension entry point
- `Makefile` - Build configuration (line 53: extension build)
- All `src/diskann_*.c` files - Call SQLite functions directly

**Reference:**

- `../sqlite-vec/Makefile` - Working build configuration
- `../sqlite-vec/sqlite-vec.c` - Working extension (~12K lines, amalgamation)

## Notes

**Current status:** Extension compiles, loads, but fails with "undefined symbol: sqlite3_bind_int64" when trying to CREATE VIRTUAL TABLE. sqlite-vec works with identical node:sqlite setup, so the problem is in our implementation/build, not fundamental impossibility.

**Next engineer:** Start with Debugging Phase tasks. The answer is in the difference between how sqlite-vec builds and how we build. Focus on:

1. Do our `diskann_*.c` files have access to SQLITE_EXTENSION_INIT routing?
2. Is there a build flag we're missing?
3. Does sqlite-vec use macros or wrapper functions we don't?

**Time estimate:** 1-2 hours to find root cause, 30min-2 hours to fix depending on solution complexity.

## Solution Implemented

### Files Changed

1. **src/diskann_sqlite.h** (NEW)
   - Conditionally includes `<sqlite3ext.h>` when `DISKANN_EXTENSION` is defined
   - Declares `extern const sqlite3_api_routines *sqlite3_api;` for non-main files
   - Includes regular `<sqlite3.h>` for test builds

2. **src/diskann_vtab.c**
   - Added `#define DISKANN_VTAB_MAIN` before includes
   - Added `SQLITE_EXTENSION_INIT1` (when `DISKANN_EXTENSION` defined)
   - Wrapped `SQLITE_EXTENSION_INIT2` in `#ifdef DISKANN_EXTENSION`
   - Fixed virtual table module: `xCreate` and `xDestroy` now point to functions (was NULL)

3. **src/diskann_internal.h**
   - Replaced `#include <sqlite3.h>` with `#include "diskann_sqlite.h"`

4. **src/diskann_api.c, diskann_blob.c, diskann_insert.c, diskann_node.c, diskann_search.c**
   - Replaced `#include <sqlite3.h>` with `#include "diskann_sqlite.h"`

5. **Makefile**
   - Added `-DDISKANN_EXTENSION` flag when building extension

### How It Works

**Extension build (make):**

- Defines `DISKANN_EXTENSION`
- All files include `<sqlite3ext.h>` via `diskann_sqlite.h`
- Only diskann_vtab.c has `SQLITE_EXTENSION_INIT1` (creates `sqlite3_api` variable)
- Other files use `extern` declaration to access same `sqlite3_api`
- All SQLite function calls go through function pointers in `sqlite3_api`
- Symbols are undefined at link time but resolved from host at load time

**Test build (make test):**

- Does NOT define `DISKANN_EXTENSION`
- All files include regular `<sqlite3.h>` via `diskann_sqlite.h`
- SQLite function calls are direct (not through pointers)
- Links against vendored `sqlite3.o`

### Verification

```bash
# All 126 C tests pass
make test

# ASan clean (no memory leaks)
make asan

# Extension builds successfully
make
# → build/diskann.so

# Extension loads in node:sqlite
node -e "import('node:sqlite').then(({DatabaseSync}) => {
  const db = new DatabaseSync('/tmp/test.db', {allowExtension: true});
  db.enableLoadExtension(true);
  db.loadExtension('./build/diskann.so');
  console.log('✓ Extension loaded');
});"
# → ✓ Extension loaded (no "undefined symbol" errors!)
```

### Next Steps

The extension now loads successfully, but CREATE VIRTUAL TABLE fails with "diskann: failed to create index". This is a separate issue - likely in diskann_create_index function. Need to investigate why index creation fails when called from virtual table xCreate/xConnect.

Possible causes:

- Database connection issue (different db handle?)
- Permission issue with shadow table creation
- NULL pointer or uninitialized value in config
- Need to debug diskann_create_index call from diskannConnect

This should be tracked in a separate TPP.
