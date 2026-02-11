# sqlite-diskann

[![npm version](https://img.shields.io/npm/v/@photostructure/sqlite-diskann.svg)](https://www.npmjs.com/package/@photostructure/sqlite-diskann)
[![CI](https://github.com/photostructure/sqlite-diskann/workflows/CI/badge.svg)](https://github.com/photostructure/sqlite-diskann/actions/workflows/ci.yml)

SQLite extension for DiskANN approximate nearest neighbor vector search.

## What is this?

A standalone SQLite extension implementing the [DiskANN algorithm](https://github.com/microsoft/DiskANN) for efficient vector similarity search at scale. Extracted from [libSQL's implementation](https://github.com/tursodatabase/libsql) and optimized for use as a standard SQLite extension.

**Key features:**

- Scales to millions of vectors
- Disk-based index using SQLite's BLOB I/O (4KB blocks)
- No separate files — entire index lives in SQLite database
- Full transactional consistency with SQLite SAVEPOINT/WAL
- Incremental insert/delete support
- Cross-platform: Linux, macOS, Windows (x64, arm64)

## Database Compatibility

This package works with multiple SQLite library implementations through duck typing:

| Library                    | Availability           | Notes                                    |
| -------------------------- | ---------------------- | ---------------------------------------- |
| **@photostructure/sqlite** | npm package            | ✅ Stable, 100% `node:sqlite` compatible |
| **better-sqlite3**         | npm package            | ✅ Mature, stable, widely used           |
| **node:sqlite**            | Node.js 22.5+ built-in | ⚠️ Experimental (requires flag)          |

### Which should I use?

- **Production**: Use `better-sqlite3` or `@photostructure/sqlite` (both stable)
- **Node 22.5+**: Can use built-in `node:sqlite` (zero dependencies, but experimental)
- **Existing projects**: Continue using whatever you already have

## Installation

```bash
# Install sqlite-diskann
npm install @photostructure/sqlite-diskann

# Install a SQLite library (choose one):

# Option 1: @photostructure/sqlite (recommended for production)
npm install @photostructure/sqlite

# Option 2: better-sqlite3 (recommended for production)
npm install better-sqlite3

# Option 3: Use Node.js 22.5+ built-in (no install needed)
# Requires Node.js >= 22.5.0 and --experimental-sqlite flag
# ⚠️ Still experimental (requires --experimental-sqlite flag)
```

## Quick Start

### With @photostructure/sqlite

```typescript
import { DatabaseSync } from "@photostructure/sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);

// Create index for 128-dimensional vectors
db.exec(`
  SELECT diskann_create('my_index', 'my_db', 128, 64, 1.2, 32);
`);

// Insert vector
const vector = new Float32Array(128);
db.prepare("SELECT diskann_insert(?, ?, ?)").run("my_index", "my_db", 1, vector);

// Search for 10 nearest neighbors
const results = db
  .prepare(
    `
  SELECT rowid, distance
  FROM diskann_search('my_index', 'my_db', ?, 10, 100)
`
  )
  .all(vector);
```

### With better-sqlite3

```typescript
import Database from "better-sqlite3";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new Database(":memory:");
loadDiskAnnExtension(db);

// Now you can use DiskANN functions
db.exec(`
  CREATE VIRTUAL TABLE embeddings USING diskann(
    dimension=512,
    metric=cosine
  )
`);
```

### With node:sqlite (Node 22.5+, experimental)

```typescript
import { DatabaseSync } from "node:sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);

// Now you can use DiskANN functions
db.exec(`
  CREATE VIRTUAL TABLE embeddings USING diskann(
    dimension=512,
    metric=cosine
  )
`);
```

## Why DiskANN?

Most SQLite vector extensions either:

- Use brute-force (doesn't scale to millions of vectors)
- Require separate index files (no transactional consistency, crash recovery)
- Have licensing restrictions (Elastic License, etc.)

DiskANN stores the entire graph index inside SQLite using shadow tables, providing true ACID guarantees and single-file databases.

See [`_research/sqlite-vector-options.md`](./_research/sqlite-vector-options.md) for comparison with alternatives.

## API Reference

### C API

```c
// Create index
int diskann_create(const char *index_name, const char *db_name,
                   int vector_dim, int max_neighbors,
                   float pruning_alpha, int search_list_size);

// Insert vector
int diskann_insert(const char *index_name, const char *db_name,
                   sqlite3_int64 rowid, const float *vector);

// Search
int diskann_search(const char *index_name, const char *db_name,
                   const float *query, int k, int search_list_size,
                   diskann_search_result **results, int *result_count);

// Delete vector
int diskann_delete(const char *index_name, const char *db_name,
                   sqlite3_int64 rowid);

// Destroy index
int diskann_destroy(const char *index_name, const char *db_name);
```

Full API: [`src/diskann.h`](./src/diskann.h)

## Building from Source

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential clang-tidy valgrind

# Build
make all

# Test
make test        # C unit tests (126 tests)
make test-stress # Stress tests (300k/100k vectors, ~30 min)
make asan        # AddressSanitizer
make valgrind    # Memory leak detection
npm test         # TypeScript tests
```

## License

MIT License

Derived from libSQL's DiskANN implementation:

- Copyright 2024 the libSQL authors
- Copyright 2026 PhotoStructure Inc.

See [LICENSE](./LICENSE) for full text.

## Links

- [DiskANN Paper (Microsoft Research)](https://proceedings.neurips.cc/paper/2019/file/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Paper.pdf)
- [libSQL DiskANN Implementation](https://github.com/tursodatabase/libsql)
- [Turso Blog: DiskANN in libSQL](https://turso.tech/blog/approximate-nearest-neighbor-search-with-diskann-in-libsql)
