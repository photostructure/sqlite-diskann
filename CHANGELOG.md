# Changelog

All notable changes to sqlite-diskann will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Dynamic `search_list_size` auto-scaling: beam width automatically increases with `sqrt(index_size)` to maintain recall at scale. No manual tuning needed for most workloads.
- Lazy back-edges optimization for deferred edge repair during batch inserts
- `diskann_begin_batch()` and `diskann_end_batch()` API for multi-insert optimization
- `diskann_abort_batch()` for transaction rollback support
- BlobSpot reference counting to prevent use-after-free at scale
- Persistent BlobCache across batch insert operations
- Insert profiling instrumentation (enabled via `DISKANN_DEBUG_TIMING=1`)
- Performance experiment documentation framework in `experiments/` directory
- Comprehensive parameter tuning guide (`PARAMETERS.md`)
- Benchmark profiles for parameter sweep testing
- Stress tests for large-scale performance validation

### Changed

- Virtual table integration now uses cache-only batch mode (lazy edges disabled for vtab path)
- Improved blob handle lifecycle management to prevent COMMIT blocking
- Enhanced experiment tracking with templates and detailed analysis requirements

### Fixed

- **Critical:** O(n) random start bottleneck reduced by 96.8% (replaced `COUNT(*)+OFFSET` with indexed seek)
- **Critical:** Auto-calculate block size based on dimensions × max_neighbors for graph connectivity
- **Critical:** BlobCache UAF segfault at 100k scale (replaced `is_cached`/`owns_blobs` with refcounting)
- Blob handle expiration during Phase 2 immediate flush (auto-reopen in `blob_spot_flush()`)
- Deferred edge repair failure due to missing `is_aborted` flag on cached blob spots
- Blob handles blocking COMMIT in virtual table path (`blob_cache_release_handles()`)
- Refcount leak in insert cleanup path (removed premature `new_blob = NULL` assignment)

### Performance

- Random start optimization: 26% → 0.9% of insert time (1.5ms → 46µs at 10k scale)
- Batch insert mode enables persistent cache across multiple inserts (0% → expected high hit rate)
- Reduced default insert list size for faster development builds

### Documentation

- Added comprehensive documentation on parameter tuning and experiment tracking
- Created benchmark framework documentation
- Added performance experiment templates and analysis guidelines
- Enhanced TypeScript API reference and usage guide
- Clarified package design for Node.js/TypeScript projects

## [0.1.2] - 2026-02-10

### Changed

- Release process refinements

## [0.1.1] - 2026-02-10

### Added

- Platform-specific binary path resolution in TypeScript wrapper
- `prepare-release` script for automated release workflow

### Fixed

- Extension loading path for platform-specific binaries

## [0.1.0] - 2026-02-10

### Added

- **Core DiskANN Implementation**
  - Complete extraction of DiskANN algorithm from libSQL
  - Public C API with 9 functions (8 original + `diskann_search_filtered`)
  - BLOB I/O layer (`src/diskann_blob.c`)
  - Node binary format with little-endian serialization (`src/diskann_node.c`)
  - Beam search implementation (`src/diskann_search.c`)
  - Insert with edge pruning and graph construction (`src/diskann_insert.c`)
  - Vector deletion support

- **Virtual Table Interface**
  - Phase 1: Basic virtual table with MATCH search
  - Phase 2: Metadata column support
  - Phase 3: Filtered search with arbitrary WHERE clauses
  - SQL-level CREATE/DROP/INSERT/SEARCH/DELETE operations

- **TypeScript/Node.js Package**
  - Hybrid CJS/ESM support for maximum compatibility
  - Duck-typed DatabaseLike interface for multi-library support
  - Type-safe TypeScript wrapper API
  - SQL injection prevention with identifier validation
  - Pre-built native binaries for supported platforms

- **Testing Infrastructure**
  - 175 total tests (126 C API + 49 virtual table)
  - Integration tests for 128D vectors
  - Recall scaling tests
  - Delete-at-scale tests
  - AddressSanitizer (ASan) verification
  - Valgrind memory leak detection
  - Stress tests for performance benchmarking

- **Build & CI/CD**
  - Cross-platform GitHub Actions workflow
  - Windows, macOS, and Linux support
  - AddressSanitizer integration
  - Valgrind integration
  - Bear (Build EAR) for compilation database
  - clang-tidy static analysis

- **Documentation**
  - Comprehensive README with installation and usage guide
  - API reference documentation
  - Project guidelines (CLAUDE.md)
  - C coding standards (DESIGN-PRINCIPLES.md)
  - TDD methodology guide (TDD.md)
  - Rust rewrite assessment (decision to stay in C)

### Fixed

- SAVEPOINT removed from index creation (prevented nested transaction issues)
- DiskAnnSearchCtx initialization to avoid undefined behavior
- Magic numbers replaced with named constants
- Windows MSVC compatibility issues
- Cross-platform timing and process ID support
- Build configuration for all platforms
- Copyright header standardization (removed "Original"/"Modifications" qualifiers)

### Changed

- Metadata stored as INTEGER (not BLOB) for cross-platform portability
- V3 node format only (no V1/V2 compatibility)
- Float32 vectors only (removed VectorPair complexity)
- pruning_alpha stored as fixed-point ×1000 in metadata
- Index/database name validation prevents SQL injection

### Known Issues

- Original libSQL bugs identified and documented:
  - `diskAnnDelete()` line 1676: uses neighbor's own rowid instead of deleted node's rowid
  - `diskAnnSearchInternal()` line 1413: `out:` label always returns SQLITE_OK, ignoring `rc`
  - `diskAnnSearchCtxInit()`: allocates with `sizeof(double)` but uses as `float*`

## [0.0.1] - 2026-02-09

### Added

- Initial project structure
- Vendored SQLite 3.51.2
- Basic Makefile
- Git repository initialization

---

## Release Notes

### Versioning Strategy

- **0.x.x**: Pre-1.0 development releases
- **1.0.0**: First stable release (planned after block size fix and batch optimization validation)

### Migration Notes

#### 0.1.0 → Unreleased

- Existing 4KB block indices may have poor recall at 100k+ scale
- Recommend rebuilding indices with auto-calculated block size
- Batch insert API is backward compatible (single inserts still work)
- Virtual table schema unchanged

### Performance Characteristics

#### Write Performance (as of unreleased)

- **Baseline:** 189 inserts/sec at 10k scale
- **With batch mode:** Pending benchmarks (expected 2-5x improvement)
- **Random start:** Now <1% of insert time (previously 26%)

#### Read Performance

- **10k vectors:** 97% recall@10
- **100k vectors:** Pending block size fix validation
- Search beam width auto-adjusted for filtered queries

### Credits

Derived from [libSQL](https://github.com/tursodatabase/libsql) DiskANN implementation (MIT license).

**Original Copyright:** 2024 the libSQL authors
**Modifications Copyright:** 2026 PhotoStructure Inc.

[Unreleased]: https://github.com/photostructure/sqlite-diskann/compare/v0.1.2...HEAD
[0.1.2]: https://github.com/photostructure/sqlite-diskann/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/photostructure/sqlite-diskann/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/photostructure/sqlite-diskann/releases/tag/v0.1.0
[0.0.1]: https://github.com/photostructure/sqlite-diskann/releases/tag/v0.0.1
