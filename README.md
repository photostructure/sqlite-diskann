# sqlite-diskann

[![npm version](https://img.shields.io/npm/v/@photostructure/sqlite-diskann.svg)](https://www.npmjs.com/package/@photostructure/sqlite-diskann)
[![CI](https://github.com/photostructure/sqlite-diskann/workflows/CI/badge.svg)](https://github.com/photostructure/sqlite-diskann/actions/workflows/ci.yml)
[![API Docs](https://img.shields.io/badge/API-Documentation-blue)](https://photostructure.github.io/sqlite-diskann/)

SQLite extension for DiskANN approximate nearest neighbor vector search.

📘 **[Full TypeScript API Documentation](https://photostructure.github.io/sqlite-diskann/)**

## What is this?

A standalone SQLite extension implementing the [DiskANN algorithm](https://github.com/microsoft/DiskANN) for efficient vector similarity search at scale. Extracted from [libSQL's implementation](https://github.com/tursodatabase/libsql) and optimized for use as a standard SQLite extension.

**Key features:**

- Scales to millions of vectors
- Disk-based index using SQLite's BLOB I/O (4KB blocks)
- No separate files — entire index lives in SQLite database
- Full transactional consistency with SQLite SAVEPOINT/WAL
- Incremental insert/delete support
- Cross-platform: Linux, macOS, Windows (x64, arm64)

**For smaller datasets** (< 100k vectors), consider [@photostructure/sqlite-vec](https://github.com/photostructure/sqlite-vec) which uses exact brute-force search and requires no index building.

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

📖 **[Complete Usage Guide](./USAGE.md)** - Detailed examples, metadata filtering, performance tips

### Basic Example

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

See [USAGE.md](./USAGE.md) for:

- Examples with better-sqlite3 and node:sqlite
- Metadata columns and filtered search
- MATCH operator syntax and query patterns
- Performance tuning and optimization tips
- C API usage

## API Reference

### TypeScript API

**📘 [Complete API Documentation](https://photostructure.github.io/sqlite-diskann/)** - Auto-generated from source with TypeDoc

The TypeScript package exports the following functions:

- **`loadDiskAnnExtension(db)`** - Load extension into database
- **`getExtensionPath()`** - Get platform-specific extension path
- **`createDiskAnnIndex(db, tableName, options)`** - Create virtual table with configuration
- **`searchNearest(db, tableName, queryVector, k)`** - Search for k nearest neighbors
- **`insertVector(db, tableName, rowid, vector)`** - Insert a vector
- **`deleteVector(db, tableName, rowid)`** - Delete a vector

See the [full API documentation](https://photostructure.github.io/sqlite-diskann/) for detailed usage, parameters, and examples.

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

## Links

- [DiskANN Paper (Microsoft Research)](https://proceedings.neurips.cc/paper/2019/file/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Paper.pdf)
- [libSQL DiskANN Implementation](https://github.com/tursodatabase/libsql)
- [Turso Blog: DiskANN in libSQL](https://turso.tech/blog/approximate-nearest-neighbor-search-with-diskann-in-libsql)
