# DiskANN Extraction from libSQL

## Summary

Extract Turso's DiskANN implementation from libSQL and create a standalone SQLite extension for ANN vector search at million-vector scale. PhotoStructure needs commercial-friendly ANN capability for CLIP embeddings (single-digit millions of vectors). Research eliminated all existing options due to abandonment, poor code quality, or licensing issues. DiskANN is production-proven (Turso), highly portable (pure C + SQLite APIs), and MIT licensed.

## Current Phase

- [x] Research & Planning
- [x] Test Design
- [x] Implementation Design
- [x] Test-First Development
- [x] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- `_research/diskann-portability.md` - Code portability analysis
- `_research/sqlite-vector-options.md` - Evaluated alternatives
- https://github.com/tursodatabase/libsql/blob/main/libsql-sqlite3/src/vectordiskann.c

## Description

**Problem:**
PhotoStructure needs ANN vector search for millions of CLIP embeddings. Current sqlite-vec fork uses brute-force (doesn't scale beyond ~100k vectors). No existing SQLite ANN extensions meet requirements: vectorlite abandoned/unproven, sqlite-vss has poor code quality, sqlite-vector requires commercial license.

**Constraints:**

- Must be standard SQLite loadable extension (not full SQLite fork)
- Must work on Linux/macOS/Windows
- Must be commercially redistributable (closed-source)
- Must handle single-digit millions of vectors
- Code quality must be maintainable (we own it long-term)

**Success Criteria:**

- Standalone extension compiles on all platforms
- No libSQL dependencies
- Handles 5M+ CLIP vectors (512-768d)
- Query latency <100ms for k-NN
- Recall rate >95%
- MIT licensed

## Tribal Knowledge

**Why extraction over alternatives:**

- sqlite-vss: "not good" C code quality (user assessment), 1GB index cap, abandoned
- vectorlite: abandoned (~1 year no commits), unproven at scale, too green
- libSQL fork: too green for production, Windows concerns, architectural change
- sqlite-vector: Elastic License 2.0 - requires commercial license

**Critical DiskANN portability findings:**

- Code is **highly portable** - analyzed vectordiskann.c
- No platform-specific dependencies (no POSIX, pthreads, Linux headers)
- Just standard C + SQLite APIs
- Explicit little-endian serialization (readLE*/writeLE*)
- Single-threaded design (no concurrency complexity)
- libSQL "Linux-only" is build system issue, NOT code issue

**Storage mechanism (CRITICAL):**

- **ALL data stored IN SQLite database** - NOT separate files!
- Uses `sqlite3_blob_*` APIs for incremental BLOB I/O
- Shadow table pattern: `{index}_shadow` with BLOB column
- Each 4KB graph node stored as BLOB row
- SQLite handles all platform-specific I/O
- **Windows I/O concerns are SOLVED** - single .db file, SQLite's page cache

**I/O pattern:**

- NOT "lots of small files" - graph nodes are BLOBs in SQLite
- <10 disk accesses per query (graph traversal)
- 4KB blocks align with NTFS clusters and SSD pages
- SQLite provides buffering, caching, transactions

**Windows validation:**

- Microsoft uses DiskANN in SQL Server 2025 on Windows
- Active optimization (RC1 improvements: faster indexing, better CPU usage)
- Proven at scale on Windows platform
- Expected latency: 1-5ms for millions of vectors

**Algorithm choice confirmed:**

- RAM constraints: 16-32GB (HNSW would need 30-45GB for 5M vectors)
- DiskANN memory: ~2-3GB for 5M vectors
- Scale: proven to billions, we need single-digit millions
- Latency: 1-5ms acceptable (vs HNSW's <1ms)

**Vector workload specifics:**

- Face vectors: 256d, <100k (brute-force sqlite-vec is fine)
- CLIP vectors: 512-768d, single-digit millions (need ANN)

**Testing framework choices (2026 research):**

- **C tests:** Unity framework (minimal, portable, actively maintained)
  - Rejected CMocka (overkill with mocking for our needs)
  - Unity: zero dependencies, ASan/Valgrind compatible, perfect for SQLite extensions
- **TypeScript tests:** Vitest (10-20x faster than Jest, native ESM/TS support)
  - Rejected Jest (slower, requires ts-jest transpiler, experimental ESM)
  - Vitest: native type testing, Jest-compatible API, industry momentum
  - Reference projects (fs-metadata, node-sqlite) use Jest but that's legacy choice
- **Test strategy documented:** tests/TEST-STRATEGY.md (dual-layer C + TS testing)

**Current extraction status:**

- 1789 lines extracted from libSQL into src/diskann.c
- Makefile with cross-platform support (Linux .so, macOS .dylib)
- ASan, bear, clang-tidy targets configured
- Still coupled to libSQL: includes `sqliteInt.h` (internal) and `vectorIndexInt.h`
- Next: decouple from libSQL internals, use only public SQLite API

**Session 2025-02-09 Discoveries:**

**Vendored SQLite (CRITICAL SUCCESS):**

- ✅ **Fully self-contained build** - zero system dependencies!
- SQLite 3.51.2 (latest 2026 version) vendored in `vendor/sqlite/`
- Includes: sqlite3.c (9MB), sqlite3.h (657KB), sqlite3ext.h (38KB)
- SQLite compiled separately with relaxed warnings (doesn't support `-Wconversion`)
- Update script created: `scripts/update-sqlite.sh` (auto-fetches latest version)
- **Why this matters:** Clone and build works immediately, consistent version everywhere, simpler CI/CD

**Build system gotchas:**

- SQLite amalgamation CANNOT compile with `-Wconversion` flag (hundreds of warnings)
- Solution: Compile SQLite separately as `build/sqlite3.o` with minimal flags
- Main code still uses strict flags: `-Wall -Wextra -Werror -pedantic -Wconversion`
- Linking: `-lpthread -ldl -lm` (no `-lsqlite3` needed)

**Test-first development setup complete:**

- Unity framework vendored in `tests/c/unity/` (3 files, ~280KB)
- Public API defined: `src/diskann.h` (8 functions, proper libSQL copyright)
- Stub implementations: `src/diskann_api.c` (all return `DISKANN_ERROR`)
- Basic tests passing: 5 tests, 0 failures
- Tests verify: functions exist, headers compile, types defined, constants available

**Copyright compliance implemented:**

- Added copyright guidelines to CLAUDE.md
- Derived files must retain: `Copyright 2024 the libSQL authors`
- Add: `Copyright 2026 PhotoStructure Inc.`
- NEVER claim sole copyright on derived code

**Implementation design completed:**

- Full decoupling strategy documented: `_research/IMPLEMENTATION-DESIGN.md`
- Key changes from libSQL:
  - Replace `sqlite3MPrintf()` → `sqlite3_mprintf()` (public API)
  - Replace `sqlite3DbFree()` → `sqlite3_free()`
  - Replace `VectorInRow/VectorOutRows` → simple `(id, vector)` tuple
  - Simplify to single-key indexing (no multi-key complexity needed)
  - Shadow table: `(id INTEGER PRIMARY KEY, data BLOB)` instead of multi-key
- New public API is **simpler** than libSQL (PhotoStructure doesn't need multi-key)

**Files created this session:**

- `src/diskann.h` - Public API header
- `src/diskann_api.c` - Stub implementations
- `tests/c/test_diskann_api.c` - Basic API tests
- `tests/TEST-STRATEGY.md` - Comprehensive test plan (C + TypeScript layers)
- `_research/IMPLEMENTATION-DESIGN.md` - Decoupling strategy and API design
- `_research/diskann-portability.md` - Portability analysis
- `scripts/update-sqlite.sh` - SQLite version updater
- Makefile updates for vendored SQLite compilation

**What worked well:**

- Vendoring SQLite source = game changer (zero dependencies)
- Test-first approach caught API design issues early
- Unity framework is perfect (minimal, portable, fast)
- Compiling SQLite separately avoids flag conflicts

**What didn't work:**

- Initially tried `-lsqlite3` (system library) - dependency hell
- Tried compiling SQLite with `-Wconversion` - hundreds of warnings
- First Makefile attempt compiled sqlite3.c with strict flags - failed

**Next session critical info:**

- **DO NOT** try to compile SQLite with project's strict warning flags
- **DO** use vendored SQLite (it's already set up correctly)
- **START** with implementing `diskann_create_index()` function
- **REFER** to `_research/IMPLEMENTATION-DESIGN.md` for decoupling strategy

**Session 2025-02-09 Part 2 - Task 1 (open/close) Complete:**

**What was done THIS session:**

- ✅ Created `src/diskann_internal.h` - Defines DiskAnnIndex struct
- ✅ Implemented diskann_open_index() - Loads metadata from database, validates index exists
- ✅ Implemented diskann_close_index() - Frees resources safely (handles NULL)
- ✅ Enhanced diskann_create_index() - Now stores config in `{index}_metadata` table
- ✅ Enhanced diskann_drop_index() - Also drops metadata table
- ✅ Created test_open_close_index.c - 9 new tests for open/close functionality
- ✅ Updated test_runner.c - Added 9 test cases (21 → 30 tests total)
- ✅ All 30 tests passing with AddressSanitizer clean

**Files created THIS session:**

- `src/diskann_internal.h` (new)
- `tests/c/test_open_close_index.c` (new)

**Files modified THIS session:**

- `src/diskann_api.c` - Added open/close implementation, metadata storage
- `tests/c/test_runner.c` - Added test declarations

**Note:** diskann_create_index(), diskann_drop_index(), and diskann_clear_index() were already implemented in a previous session. This session only added the open/close functionality and metadata storage.

**Session 2025-02-09 Part 3 - Task 8 (BLOB I/O) Complete:**

**What was done THIS session:**

- ✅ Created `src/diskann_blob.h` - BLOB I/O API with BlobSpot structure
- ✅ Created `src/diskann_blob.c` - Full implementation extracted from libSQL
- ✅ Updated `src/diskann_internal.h` - Added num_reads/num_writes statistics
- ✅ Created `tests/c/test_blob_io.c` - 7 comprehensive tests
- ✅ Updated Makefile - Added diskann_blob.c to test build
- ✅ All 37 tests passing (up from 30), AddressSanitizer clean

**BLOB I/O features implemented:**

- blob_spot_create() - Open BLOB handle, allocate buffer
- blob_spot_reload() - Reuse handle for different rowids (optimization)
- blob_spot_flush() - Write buffer back to database
- blob_spot_free() - Clean up resources safely
- Error handling - Distinguishes "row not found" from generic errors
- Abort recovery - Handles SQLITE_ABORT by closing/reopening

**Files created THIS session:**

- `src/diskann_blob.h` (new)
- `src/diskann_blob.c` (new)
- `tests/c/test_blob_io.c` (new)

**Files modified THIS session:**

- `src/diskann_internal.h` - Added statistics fields
- `src/diskann_api.c` - Initialize statistics in diskann_open_index()
- `tests/c/test_runner.c` - Added 7 BLOB test declarations
- `Makefile` - Include diskann_blob.c in test build

**Design decisions:**

- Used sqlite3_malloc/sqlite3_free for consistency with SQLite
- Reuse BLOB handles via sqlite3_blob_reopen() for performance
- Track num_reads/num_writes for debugging/profiling
- Safe with NULL (blob_spot_free can handle NULL)

**Critical path complete:**

- Tasks 7 & 8 unblock Tasks 4-6 (insert/search/delete)
- Next: Implement diskann_insert() (Task 4)

**Session 2025-02-09 Part 4 - Code Review Fixes:**

**Bugs fixed in diskann_api.c:**

1. **SQL injection via index_name:** Added `validate_identifier()` — names must match
   `[a-zA-Z_][a-zA-Z0-9_]{0,63}`. Applied to all 5 public functions.
2. **Platform-dependent metadata:** Changed from native-endian BLOBs to portable SQLite
   INTEGERs. Metadata table schema now `value INTEGER NOT NULL` (was `value BLOB`).
   Added `store_metadata_int()` helper. Human-readable via `sqlite3` CLI.
3. **Fragile multi-row INSERT:** Replaced single 6-row `INSERT OR REPLACE` with individual
   `store_metadata_int()` calls per key. Clearer, less error-prone.

**Also fixed:** `diskann_drop_index()` and `diskann_clear_index()` sqlite_master queries
now use `"%w"` for db_name (was missing schema qualification).

**Tests added:** `test_create_index_invalid_name` + `test_metadata_roundtrip` (37 → 39 tests)
**All 39 tests pass:** `make test`, `make asan`, `make valgrind` all clean.

**Session 2025-02-09 Part 5 - Task 7 Edge Case Tests Complete:**

**What was done THIS session:**

- ✅ Added `test_blob_spot_flush_readonly()` - Verifies DISKANN_ERROR_INVALID when flushing read-only BlobSpot
- ✅ Added `test_blob_spot_create_null_output()` - Verifies DISKANN_ERROR_INVALID with NULL output pointer
- ✅ Added `test_clear_index_preserves_metadata()` - Verifies metadata table survives clear, values intact
- ✅ Updated test_runner.c - Added 3 test declarations and RUN_TEST calls
- ✅ All 46 tests passing (up from 39), ASan + Valgrind clean

**Files modified THIS session:**

- `tests/c/test_blob_io.c` - Added 2 edge case tests (flush readonly, NULL output)
- `tests/c/test_drop_clear_index.c` - Added metadata preservation test + helper functions
- `tests/c/test_runner.c` - Added 3 test declarations and runner calls
- `_todo/20250209-diskann-extraction.md` - Updated Task 7 status, test counts

**Task 7 STATUS:** ✅ COMPLETE (all edge case tests implemented and passing)

## Solutions

### Option 1: Extract DiskANN from libSQL ⭐ CHOSEN

**Pros:**

- Production-proven at billions of vectors (Turso)
- Clean, portable C code (no platform deps)
- MIT licensed
- Disk-based (no memory caps)
- No C++ dependencies

**Cons:**

- Medium extraction effort (2-3 weeks)
- Need to decouple from libSQL infrastructure
- Shadow table system needs reimplementation

**Status:** Chosen - best combination of code quality, scalability, and licensing

### Option 2: Fork sqlite-vss (Faiss-based)

**Pros:**

- Faiss proven to billions
- MIT licensed
- Existing wrapper code

**Cons:**

- "Not good" C code quality (dealbreaker for long-term maintenance)
- 1GB index cap to remove
- C++ dependency (Faiss)
- Abandoned by author

**Status:** Rejected - poor code quality

### Option 3: Fork vectorlite (HNSW-based)

**Pros:**

- HNSW implementation exists
- Apache 2.0 licensed
- Simpler than Faiss

**Cons:**

- Abandoned (~1 year no commits)
- Unproven at million+ scale
- No benchmarks beyond 20k vectors

**Status:** Rejected - too risky, unproven scale

## Tasks

### Phase 1: Extraction & Analysis ✅ COMPLETE

- [x] Create `photostructure/sqlite-diskann` repository (local, not pushed yet)
- [x] Extract vectordiskann.c from libSQL (1789 lines in src/diskann.c)
- [x] Extract dependencies analysis (documented in IMPLEMENTATION-DESIGN.md)
- [x] Document all libSQL-specific APIs used (see \_research/IMPLEMENTATION-DESIGN.md)
- [x] Map coupling points (VectorInRow, VectorOutRows, sqlite3MPrintf, etc.)
- [x] Create extension skeleton with build system (Makefile with asan/valgrind/bear)

### Phase 2: Decoupling ✅ COMPLETE

- [x] Design new public API (src/diskann.h - 8 functions)
- [x] Create stub implementations (src/diskann_api.c)
- [x] Set up test infrastructure (Unity framework)
- [x] Write comprehensive tests (30 tests, all passing)
- [x] Implement diskann_create_index() ✅ (with metadata table storage)
- [x] Implement diskann_drop_index() ✅ (drops shadow + metadata tables)
- [x] Implement diskann_clear_index() ✅
- [x] Implement diskann_open_index() ✅ (loads metadata, validates index exists)
- [x] Implement diskann_close_index() ✅ (proper resource cleanup, safe with NULL)
- [x] **TASK 7:** Define internal types (diskann_internal.h) ✅ COMPLETE
- [x] **TASK 8:** Implement BLOB I/O layer (diskann_blob.c) ✅ COMPLETE
- [x] **TASK 4:** Implement diskann_insert() (graph construction) ✅ COMPLETE
- [x] **TASK 5:** Implement diskann_search() (k-NN search) ✅ COMPLETE
- [x] **TASK 6:** Implement diskann_delete() (graph update) ✅ COMPLETE
- [x] Replace libSQL vector types with DiskAnnVector ✅
- [x] Port parameter management (simplified DiskAnnConfig) ✅
- [x] Remove all libSQL dependencies (replace sqlite3MPrintf, etc.) ✅

### Phase 3: Cross-Platform Build

- [x] Linux build (gcc/clang) - working locally
- [x] macOS build (x86_64 and ARM64) - CI jobs: test-macos-arm64, test-macos-x64
- [x] Windows build (MSVC) - CI jobs: test-windows-x64, test-windows-arm64 via scripts/build-windows.ps1
- [x] **TASK 10:** Setup GitHub Actions CI/CD (follow fs-metadata pattern) ✅ 6 platform jobs + lint + TypeScript + publish
- [x] Create prebuilt binaries - artifacts staged per-platform in CI (prebuilds/{os}-{arch}/)
- [ ] Verify loadable in SQLite on all platforms

### Phase 4: Testing

- [x] Basic operations (create/insert/query/delete) - 126 tests covering all 8 API functions
- [x] Correctness tests (compare to brute-force) - test_search_brute_force_recall, test_insert_recall
- [ ] Scale tests (1M, 3M, 5M, 10M vectors)
- [ ] Performance benchmarks (query latency, index size)
- [x] Memory leak checks (Valgrind, ASan) - CI runs ASan on 4 platforms, Valgrind on 2

### Phase 5: Integration

- [x] **TASK 3:** Create npm package structure (follow fs-metadata, NOT sqlite-vec) ✅
- [x] **TASK 9:** Integration tests (create → insert → search workflow) ✅ COMPLETE
- [x] **TASK 11:** TypeScript wrapper and type definitions ✅
- [ ] PhotoStructure integration
- [ ] Migration for existing CLIP embeddings
- [ ] Production rollout plan

**Verification:**

```bash
# Current tests
make test          # 126 tests, 0 failures
make asan          # AddressSanitizer checks
make valgrind      # Memory leak checks
```

**Build commands:**

```bash
make clean         # Clean build artifacts
make test          # Build and run tests
make asan          # Run with AddressSanitizer
make valgrind      # Run with Valgrind (comprehensive)
make bear          # Generate compile_commands.json
make clang-tidy    # Static analysis (after 'make bear')
make help          # Show all targets
```

## Notes

**Timeline estimate:** 2-3 weeks to proof-of-concept

**Key files to extract:**

- `libsql-sqlite3/src/vectordiskann.c` - Main implementation
- `libsql-sqlite3/src/vectorIndexInt.h` - Vector index interface
- Related vector infrastructure from libSQL

**Research artifacts:**

- `_research/sqlite-vector-options.md` - Full comparison matrix of all options
- `_research/diskann-portability.md` - Code portability analysis
- `_research/algorithm-comparison.md` - DiskANN vs HNSW vs Faiss analysis
- `_research/diskann-windows-performance.md` - Windows I/O validation
- `/home/mrm/.claude/plans/giggly-plotting-sutton.md` - Complete research notes

**Key I/O functions to extract:**

- `sqlite3_blob_open()` - Open BLOB handle for graph node
- `sqlite3_blob_read()` - Read node data into buffer
- `sqlite3_blob_write()` - Write modified node back
- `sqlite3_blob_close()` - Close handle
- `sqlite3_blob_reopen()` - Access different node (optimization)

**Shadow table schema (from libSQL):**

```sql
CREATE TABLE {index_name}_shadow (
  id INTEGER PRIMARY KEY,
  data BLOB  -- 4KB graph node data
);
```

---

## Parallel Work Breakdown (4 Engineers)

### ✅ COMPLETED (30/30 Tests Passing!)

- **diskann_create_index()** - Shadow table creation ✅
- **diskann_drop_index()** - Drop shadow table ✅
- **diskann_clear_index()** - Clear shadow table data ✅
- **diskann_open_index()** - Load index from database ✅
- **diskann_close_index()** - Free resources ✅

**Progress:** Tasks 1-2 DONE! 🎉

### 🔴 CRITICAL PATH (Must Complete Before Insert/Search)

**TASK 7: Port Internal Types & Add Edge Case Tests** ✅ COMPLETE

- **File:** `src/diskann_internal.h` ✅ CREATED
- **Status:** DiskAnnIndex struct fully defined. DiskAnnNode and DiskAnnSearchCtx
  will be ported when implementing Tasks 4-6 (they already exist in libSQL code).
- **Edge case tests added (Session 2025-02-09 Part 5):**
  - `test_blob_spot_flush_readonly` - Verifies flush fails on read-only BlobSpot
  - `test_blob_spot_create_null_output` - Verifies creation fails with NULL output pointer
  - `test_clear_index_preserves_metadata` - Verifies metadata table survives clear operation
- **Test count:** 46 tests (up from 39), all passing with ASan + Valgrind clean

---

**TASK 8: BLOB I/O Layer** ✅ COMPLETE

- **Files:** `src/diskann_blob.c` + `src/diskann_blob.h`
- **Tests:** `tests/c/test_blob_io.c` (9 tests, including 2 new edge case tests)
- **Status:** Fully extracted from libSQL with abort recovery, handle reuse, and I/O stats

---

### ✅ COMPLETED TASKS

**TASK 1: Open/Close Index** ✅ COMPLETE

- Implemented in `src/diskann_api.c`, tests in `tests/c/test_open_close_index.c`

**TASK 3: npm Package Structure** ✅ COMPLETE

- TypeScript wrapper, Vitest tests, tsconfig.json, .prettierrc
- 7 skipped tests waiting for C extension binary
- Reference: `../fs-metadata` pattern (NOT sqlite-vec)

**TASK 11: TypeScript Wrapper and Type Definitions** ✅ COMPLETE

- `src/index.ts` (233 lines): Complete API wrapper with platform-specific binary loading
  - `getExtensionPath()`, `loadDiskAnnExtension()`, `createDiskAnnIndex()`
  - `insertVector()`, `searchNearest()`, `deleteVector()`
  - SQL injection protection via `isValidIdentifier()` validation
  - Full input validation with descriptive errors
  - Comprehensive JSDoc documentation
- `src/types.ts` (69 lines): Full type definitions
  - `DiskAnnIndexOptions`, `NearestNeighborResult`, `DistanceMetric`, `SearchOptions`
- `tests/ts/api.test.ts` (297 lines): 16 tests for API validation
  - Parameter validation, platform paths, error handling, type exports
  - 7 tests skipped with `.skip()` awaiting C extension binary
- All 15/15 non-skipped tests passing ✅

---

### 🔴 HIGH COMPLEXITY (Require Coordination)

**TASK 4: Insert Vector** (Complexity: 🔴 HIGH, Impact: 🔴 CRITICAL)

- **File:** `src/diskann_insert.c` (new, extracted from diskann.c)
- **What:** Insert vector, build graph, write BLOBs
- **Depends on:** Tasks 7, 8
- **Owner:** Recommend pairing/review

**Implementation:**

- Extract libSQL's `diskAnnInsert()` (line ~1495-1630)
- Graph construction algorithm
- Find k-nearest neighbors
- Add bidirectional edges
- Write 4KB graph nodes via BLOB I/O

**Complexity:** Core ANN algorithm, needs careful extraction

---

**TASK 5: Search k-NN** (Complexity: 🔴 HIGH, Impact: 🔴 CRITICAL)

- **File:** `src/diskann_search.c` (new, extracted from diskann.c)
- **What:** k-NN search via graph traversal
- **Depends on:** Tasks 7, 8
- **Owner:** Recommend pairing/review

**Implementation:**

- Extract libSQL's `diskAnnSearch()` (line ~1426-1490)
- Beam search through graph
- Read graph nodes via BLOB I/O
- Distance calculations
- Return sorted results

**Complexity:** Core ANN algorithm, performance-critical

---

**TASK 6: Delete Vector** (Complexity: 🟡 MEDIUM)

- **File:** `src/diskann_api.c`
- **What:** Remove vector, update graph edges
- **Depends on:** Tasks 7, 8
- **Owner:** Unassigned

**Implementation:**

- Delete row from shadow table
- Update neighbor connections (graph repair)
- Can be basic initially (just delete, don't repair)

---

### 🟢 LATER (After Core Functions)

**TASK 9: Integration Tests**

- Full workflow: create → insert many → search → delete
- Correctness: compare vs brute-force
- Performance: benchmark query latency

**TASK 10: CI/CD**

- GitHub Actions
- Cross-platform builds (Linux, macOS, Windows)
- ASan/Valgrind in CI

**TASK 11: Documentation**

- API documentation
- Usage examples
- Migration guide from sqlite-vec

---

## Recommended Assignment (4 Engineers)

**COMPLETED:** All tasks (1-9, 11) ✅

All 8 public API functions implemented, 126 tests passing (ASan + Valgrind clean).
Integration tests cover create → insert → search → delete workflow at 128D.

---

## Current Test Status

```bash
make test
# 126 Tests 0 Failures 0 Ignored
# OK ✅
# ASan: clean, Valgrind: 0 leaks 0 errors
```

**Tests Passing (126 total):**

- ✅ API existence (5 tests)
- ✅ Create index (10 tests)
- ✅ Drop index (4 tests)
- ✅ Clear index (6 tests, including metadata preservation)
- ✅ Open/Close index (11 tests)
- ✅ BLOB I/O (9 tests, including readonly flush + NULL output edge cases)
- ✅ LE serialization (5 tests)
- ✅ Layout (4 tests)
- ✅ Node binary (9 tests)
- ✅ Distance (8 tests)
- ✅ Buffer management (10 tests)
- ✅ Node alloc (2 tests)
- ✅ Derived fields (1 test)
- ✅ Search validation (6 tests), empty (1), single-vector (3), known-graph (6), recall (1), cosine (1)
- ✅ Delete (8 tests)
- ✅ Insert (11 tests)
- ✅ Integration (4 tests) — reopen persistence, clear+reinsert, 128D recall, delete at scale

**Functions Implemented:** 8 of 8 (100%) ✅

- ✅ diskann_create_index()
- ✅ diskann_open_index()
- ✅ diskann_close_index()
- ✅ diskann_drop_index()
- ✅ diskann_clear_index()
- ✅ diskann_insert()
- ✅ diskann_search()
- ✅ diskann_delete()

**BLOB I/O Layer:** ✅ COMPLETE

- ✅ blob_spot_create()
- ✅ blob_spot_reload()
- ✅ blob_spot_flush()
- ✅ blob_spot_free()
