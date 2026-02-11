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

### Virtual Table Interface (Recommended)

The virtual table interface provides standard SQL operations with full query planner integration:

```typescript
import { DatabaseSync } from "@photostructure/sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);

// Create virtual table for 128-dimensional vectors
db.exec(`
  CREATE VIRTUAL TABLE embeddings USING diskann(
    dimension=128,
    metric=cosine
  )
`);

// Insert vectors with explicit rowid
const vector = new Float32Array(128);
db.prepare("INSERT INTO embeddings(rowid, vector) VALUES (?, ?)").run(1, vector);

// Search for 10 nearest neighbors using MATCH operator
const results = db
  .prepare(
    `
    SELECT rowid, distance
    FROM embeddings
    WHERE vector MATCH ? AND k = 10
  `
  )
  .all(vector);

// Delete a vector
db.prepare("DELETE FROM embeddings WHERE rowid = ?").run(1);

// Drop the entire index
db.exec("DROP TABLE embeddings");
```

**Virtual table features**:

- Standard SQL INSERT/DELETE/DROP operations
- MATCH operator for ANN search with `k` parameter
- LIMIT support for capping results
- Automatic shadow table management
- Full transactional consistency

### With better-sqlite3

```typescript
import Database from "better-sqlite3";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new Database(":memory:");
loadDiskAnnExtension(db);

// Create virtual table
db.exec(`
  CREATE VIRTUAL TABLE embeddings USING diskann(
    dimension=512,
    metric=cosine
  )
`);

// Insert and search work the same as above
const vector = new Float32Array(512);
db.prepare("INSERT INTO embeddings(rowid, vector) VALUES (?, ?)").run(1, vector);

const results = db
  .prepare("SELECT rowid, distance FROM embeddings WHERE vector MATCH ? AND k = 10")
  .all(vector);
```

### With node:sqlite (Node 22.5+, experimental)

```typescript
import { DatabaseSync } from "node:sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);

// Create virtual table
db.exec(`
  CREATE VIRTUAL TABLE embeddings USING diskann(
    dimension=512,
    metric=cosine
  )
`);

// Insert and search work the same as above
const vector = new Float32Array(512);
db.prepare("INSERT INTO embeddings(rowid, vector) VALUES (?, ?)").run(1, vector);

const results = db
  .prepare("SELECT rowid, distance FROM embeddings WHERE vector MATCH ? AND k = 10")
  .all(vector);
```

### C API (Advanced)

For direct C API usage, the lower-level functions are still available:

```c
// Create index
diskann_create_index(db, "main", "my_index", &config);

// Open index
DiskAnnIndex *idx;
diskann_open_index(db, "main", "my_index", &idx);

// Insert vector
diskann_insert(idx, rowid, vector, dims);

// Search
DiskAnnResult results[10];
int count = diskann_search(idx, query, dims, 10, results);

// Close
diskann_close_index(idx);
```

See [`src/diskann.h`](./src/diskann.h) for full C API documentation.

## Why DiskANN?

Most SQLite vector extensions either:

- Use brute-force (doesn't scale to millions of vectors)
- Require separate index files (no transactional consistency, crash recovery)
- Have licensing restrictions (Elastic License, etc.)

DiskANN stores the entire graph index inside SQLite using shadow tables, providing true ACID guarantees and single-file databases.

See [`_research/sqlite-vector-options.md`](./_research/sqlite-vector-options.md) for comparison with alternatives.

## API Reference

### Virtual Table SQL

```sql
-- Create index for N-dimensional vectors
CREATE VIRTUAL TABLE table_name USING diskann(
  dimension=N,              -- Required: vector dimensionality
  metric=euclidean|cosine|dot,  -- Optional: distance metric (default: cosine)
  max_degree=64,            -- Optional: max graph degree (default: 64)
  build_search_list_size=100    -- Optional: search quality (default: 100)
);

-- Insert vector (rowid required, no auto-increment)
INSERT INTO table_name(rowid, vector) VALUES (?, ?);

-- Search for k nearest neighbors using MATCH operator
SELECT rowid, distance
FROM table_name
WHERE vector MATCH ? AND k = ?
LIMIT ?;  -- Optional: caps result count

-- Delete vector
DELETE FROM table_name WHERE rowid = ?;

-- Drop entire index
DROP TABLE table_name;
```

### C API

For advanced usage, see [`src/diskann.h`](./src/diskann.h) for the full C API.

## Building from Source

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential clang-tidy valgrind

# Build
make all

# Test
make test        # C unit tests
make test-stress # Stress tests (~30 min)
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
