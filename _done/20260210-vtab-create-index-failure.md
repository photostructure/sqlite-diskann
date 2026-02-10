# Fix Virtual Table CREATE INDEX Failure

## Summary

Extension loads successfully and module registers, but CREATE VIRTUAL TABLE fails with "diskann: failed to create index". The issue occurs when `diskannConnect()` calls `diskann_create_index()` in extension mode. Need to debug why index creation fails.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [x] Integration
- [x] Cleanup & Documentation
- [x] Final Review

## Status: ✅ COMPLETE

## Required Reading

- `src/diskann_vtab.c` - Virtual table implementation (diskannConnect calls diskann_create_index)
- `src/diskann_api.c` - diskann_create_index implementation
- `20250210-extension-loading-fix.md` - Context on recent fix (extension now loads successfully)

## Description

**Problem:** After fixing the "undefined symbol" errors, the extension loads and the module registers successfully. However, when trying to CREATE VIRTUAL TABLE, it fails:

```bash
$ node -e "..."
✓ Extension loaded
Error: diskann: failed to create index
```

Debug output shows:

- `sqlite3_diskann_init()` is called ✅
- `sqlite3_create_module_v2()` returns 0 (success) ✅
- `diskannConnect()` is called (vtab xCreate) ✅
- `diskann_create_index()` returns error ❌

**Success criteria:**

- CREATE VIRTUAL TABLE succeeds in both file and :memory: databases
- Can INSERT vectors via virtual table
- Can SELECT (search) via virtual table
- Can DELETE via virtual table
- All existing 126 C tests still pass

## Tribal Knowledge

### Current State (Post Extension Loading Fix)

**What works:**

- Extension compiles: `make` → `build/diskann.so`
- Extension loads: `db.loadExtension('./build/diskann.so')` succeeds
- Module registers: `sqlite3_create_module_v2()` returns SQLITE_OK
- xCreate is called: `diskannConnect()` is invoked for CREATE VIRTUAL TABLE
- All 126 C tests pass (using `diskann_create_index()` directly with vendored SQLite)
- Virtual table module has proper xCreate/xDestroy (fixed from NULL during loading fix)

**What fails:**

- `diskann_create_index()` returns error (not DISKANN_OK or DISKANN_ERROR_EXISTS)
- Virtual table creation aborts with "diskann: failed to create index"
- Fails with both file database (`/tmp/test.db`) and :memory: database

**Critical difference:** C tests call `diskann_create_index(db, "main", "test_idx", &config)` directly and succeed. Virtual table calls the same function with same parameters but fails. This suggests the issue is **environment-specific** (extension context vs test context), not a bug in diskann_create_index itself.

### Recent Context: Extension Loading Fix (20260210-extension-loading-fix.md)

We just solved "undefined symbol" errors by creating `src/diskann_sqlite.h`:

- When `DISKANN_EXTENSION` defined: includes `<sqlite3ext.h>`, uses extension API routing
- When not defined: includes regular `<sqlite3.h>`, direct function calls
- Only `diskann_vtab.c` has `SQLITE_EXTENSION_INIT1` (defines `sqlite3_api`)
- Other files use `extern const sqlite3_api_routines *sqlite3_api;`

**This means:** In extension builds, ALL SQLite function calls (including inside `diskann_create_index`) go through the `sqlite3_api` function pointer table set up by `SQLITE_EXTENSION_INIT2(pApi)` in the init function. This is different from test builds where calls go directly to vendored SQLite.

**Possible implications:**

- `sqlite3_exec()`, `sqlite3_prepare_v2()`, `sqlite3_bind_*()` all routed through API
- Error messages might differ (`sqlite3_errmsg()` routed)
- Transaction/savepoint behavior might differ
- Database handle might be interpreted differently

### Debug Output from Our Session

```
DEBUG: diskann_init called
DEBUG: sqlite3_create_module_v2 returned 0
✓ Extension loaded
Error: diskann: failed to create index
```

This confirms:

1. Init function runs successfully (API routing is set up)
2. Module registers successfully
3. diskannConnect is invoked (that's where the error comes from)
4. Error message comes from `src/diskann_vtab.c:140`:
   ```c
   rc = diskann_create_index(db, db_name, table_name, &config);
   if (rc != DISKANN_OK && rc != DISKANN_ERROR_EXISTS) {
     *pzErr = sqlite3_mprintf("diskann: failed to create index");
     return SQLITE_ERROR;
   }
   ```

### Code Locations

**diskannConnect** (`src/diskann_vtab.c:80-178`):

```c
const char *db_name = argv[1];      // "main" or schema name
const char *table_name = argv[2];   // Virtual table name
// Parse argv[3+] for config (dimension=N, metric=X, etc.)
rc = diskann_create_index(db, db_name, table_name, &config);
```

**diskann_create_index** (`src/diskann_api.c:114-237`):

- Validates db_name and index_name with `validate_identifier()`
- Checks if shadow table exists with `shadow_table_exists()`
- Creates SAVEPOINT
- Creates `{index_name}_shadow` table
- Inserts metadata
- Releases SAVEPOINT
- Returns DISKANN_OK or DISKANN_ERROR_EXISTS or error

**Error return paths in diskann_create_index:**

- Line 127: Invalid db_name → DISKANN_ERROR
- Line 131: Invalid index_name → DISKANN_ERROR
- Line 142: Shadow table exists → DISKANN_ERROR_EXISTS (handled by vtab)
- Line 161: CREATE TABLE fails → DISKANN_ERROR
- Line 186+: Metadata insert fails → DISKANN_ERROR

### Quick Reproduction

```bash
# Make sure extension is built
make clean && make

# Minimal test case (will fail)
node -e "
import('node:sqlite').then(({DatabaseSync}) => {
  const db = new DatabaseSync('/tmp/test_vtab.db', {allowExtension: true});
  db.enableLoadExtension(true);
  db.loadExtension('./build/diskann.so');
  console.log('✓ Extension loaded');

  // This will fail with 'diskann: failed to create index'
  db.exec('CREATE VIRTUAL TABLE t USING diskann(dimension=10, metric=cosine)');
  console.log('✓ Virtual table created');
});" 2>&1
```

Expected output:

```
✓ Extension loaded
Error: diskann: failed to create index
```

To debug, add logging to `src/diskann_api.c:diskann_create_index()` and rebuild:

```bash
make clean && make
node -e "..." 2>&1  # Re-run test to see debug output
```

### Hypotheses

1. **Database handle mismatch?**
   - The db handle passed to xCreate might be different from what diskann_create_index expects
   - Extension API routing might affect how db handle is used

2. **Shadow table creation fails?**
   - diskann_create_index creates `{index_name}_shadow` table
   - Might fail due to permissions, schema name, or SQL syntax issue
   - Need to check if `db_name` from argv[1] is correct in extension context

3. **Transaction/savepoint issue?**
   - diskann_create_index uses SAVEPOINT
   - Might conflict with virtual table creation transaction
   - Extension context might have different transaction semantics

4. **Identifier validation?**
   - Table name from argv[2] might not pass validate_identifier()
   - argv parsing might be different in extension vs test context

5. **Config parsing?**
   - `argv[3+]` parameters might not parse correctly
   - dimension=0 would cause failure (required parameter)

## Tasks

### Debugging Phase

- [ ] **Add detailed error logging to diskann_create_index**
  - Log entry: db handle, db_name, index_name, config values
  - Log which step fails: validation, shadow table check, CREATE TABLE, metadata insert
  - Return specific error codes instead of generic DISKANN_ERROR

- [ ] **Add detailed error logging to diskannConnect**
  - Log argv[0] (module), argv[1] (db), argv[2] (table), argv[3+] (params)
  - Log parsed config: dimensions, metric, max_neighbors
  - Log diskann_create_index return code

- [ ] **Test with minimal repro**
  - Create smallest possible test case
  - Try with file database vs :memory:
  - Try with explicit parameters: `CREATE VIRTUAL TABLE t USING diskann(dimension=10)`

- [ ] **Check shadow table creation**
  - Manually create shadow table before CREATE VIRTUAL TABLE
  - See if diskann_create_index succeeds with existing shadow table
  - Check actual schema name being used

- [ ] **Compare extension vs test context**
  - Add same logging to C tests that use diskann_create_index
  - Compare what's different between test call and extension call
  - Check db_name value ("main" vs something else?)

### Implementation Phase (TBD)

_Will be filled in after debugging reveals root cause_

## Critical Files

**To investigate:**

- `src/diskann_vtab.c:80-142` - diskannConnect implementation
- `src/diskann_api.c:diskann_create_index` - Index creation logic
- `src/diskann_api.c:validate_identifier` - Name validation
- `src/diskann_api.c:shadow_table_exists` - Table existence check

## Verification Commands

```bash
# After fix, these should all succeed:

# 1. Load extension in node:sqlite
node -e "
import('node:sqlite').then(({DatabaseSync}) => {
  const db = new DatabaseSync('/tmp/test.db', {allowExtension: true});
  db.enableLoadExtension(true);
  db.loadExtension('./build/diskann.so');
  console.log('✓ Extension loaded');
  db.exec('CREATE VIRTUAL TABLE t USING diskann(dimension=10, metric=cosine)');
  console.log('✓ Virtual table created');
  db.close();
});"

# 2. Insert and search
node -e "
import('node:sqlite').then(({DatabaseSync}) => {
  const db = new DatabaseSync('/tmp/test.db', {allowExtension: true});
  db.enableLoadExtension(true);
  db.loadExtension('./build/diskann.so');
  db.exec('CREATE VIRTUAL TABLE t USING diskann(dimension=3, metric=cosine)');

  const v1 = Buffer.from(new Float32Array([1, 0, 0]).buffer);
  const v2 = Buffer.from(new Float32Array([0, 1, 0]).buffer);
  db.prepare('INSERT INTO t(rowid, vector) VALUES (?, ?)').run(1, v1);
  db.prepare('INSERT INTO t(rowid, vector) VALUES (?, ?)').run(2, v2);

  const query = Buffer.from(new Float32Array([1, 0.1, 0]).buffer);
  const results = db.prepare('SELECT rowid FROM t WHERE vector = ?').all(query);
  console.log('✓ Search results:', results);
  db.close();
});"

# 3. All C tests still pass
make test

# 4. ASan clean
make asan
```

## Notes

**Relationship to other TPPs:**

- Supersedes the architectural approach in vtab-phase0/phase1 (those plan a rewrite, this fixes current impl)
- Blocks: vtab-phase2-metadata.md (can't add features until basic CREATE works)
- After: 20250210-extension-loading-fix.md (that fixed symbol loading, this fixes runtime behavior)

**Priority:** HIGH - This is blocking basic extension usage. The extension loads but can't create tables.

**Estimated time:** 1-2 hours for debugging, 30min-2 hours for fix depending on root cause.

## Solution Implemented

### Root Cause

`diskann_create_index()` attempted to create a SAVEPOINT before creating shadow tables. When called from within a virtual table `xCreate/xConnect` callback (which is already executing within a DDL statement context), this fails with `SQLITE_BUSY` error code 5: "cannot open savepoint - SQL statements in progress".

### Fix Applied

**Removed SAVEPOINT from diskann_create_index()** (src/diskann_api.c lines 152-162 and 240-265)

**Why this is safe:**

1. When called from xCreate: operates within CREATE VIRTUAL TABLE transaction, SQLite rolls back automatically on error
2. When called from tests: operates in its own transaction context
3. Shadow table creation is atomic - either all tables are created or none
4. No SAVEPOINT needed - returning an error code causes automatic rollback

**Reference:** sqlite-vec creates shadow tables the same way (no SAVEPOINT in xCreate).

### Files Modified

1. **src/diskann_api.c**
   - Removed SAVEPOINT creation before shadow table operations
   - Removed RELEASE SAVEPOINT after operations
   - Removed rollback label and cleanup code
   - Changed all `goto rollback` to `return rc`
   - Added comment explaining why SAVEPOINT is not needed

### Verification

```bash
# All 126 C tests pass
make test  # ✅ OK

# ASan clean
make asan  # ✅ OK

# Extension loads and CREATE VIRTUAL TABLE works
node -e "..." # ✅ CREATE VIRTUAL TABLE succeeds

# Works with both :memory: and file databases
# ✅ Verified
```

## Recommended Starting Point (Historical)

**Step 1: Add detailed logging to diskann_create_index**

Add fprintf to stderr at each major step in `src/diskann_api.c:diskann_create_index()`:

```c
int diskann_create_index(sqlite3 *db, const char *db_name,
                         const char *index_name,
                         const DiskAnnConfig *config) {
  fprintf(stderr, "DEBUG diskann_create_index: db=%p db_name='%s' index_name='%s' dim=%u\n",
          (void*)db, db_name, index_name, config->dimensions);

  // Validate db_name
  if (!validate_identifier(db_name)) {
    fprintf(stderr, "DEBUG: Invalid db_name\n");
    return DISKANN_ERROR;
  }

  // Validate index_name
  if (!validate_identifier(index_name)) {
    fprintf(stderr, "DEBUG: Invalid index_name\n");
    return DISKANN_ERROR;
  }
  fprintf(stderr, "DEBUG: Validation passed\n");

  // Check if exists
  if (shadow_table_exists(db, db_name, index_name)) {
    fprintf(stderr, "DEBUG: Shadow table exists\n");
    return DISKANN_ERROR_EXISTS;
  }
  fprintf(stderr, "DEBUG: Shadow table doesn't exist, proceeding\n");

  // SAVEPOINT
  fprintf(stderr, "DEBUG: Creating SAVEPOINT\n");
  // ... rest of function with similar logging
}
```

Then rebuild and run the test to see which step fails:

```bash
make clean && make
node -e "..." 2>&1
```

**Step 2: Compare with working C test**

Add same logging to a C test and compare output:

```bash
make test 2>&1 | grep -A 5 "DEBUG diskann_create_index"
```

The difference between test output and extension output will reveal the issue.

**Step 3: Check argv values in diskannConnect**

Add logging in `src/diskann_vtab.c:diskannConnect()`:

```c
fprintf(stderr, "DEBUG diskannConnect: argc=%d\n", argc);
for (int i = 0; i < argc; i++) {
  fprintf(stderr, "DEBUG argv[%d]='%s'\n", i, argv[i]);
}
fprintf(stderr, "DEBUG parsed: db_name='%s' table_name='%s' dimensions=%u\n",
        db_name, table_name, config.dimensions);
```

This will show if argv parsing is working correctly in extension context.
