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

## Metadata Columns and Filtered Search

Add metadata columns to enable filtered vector search. Filters are evaluated **during** graph traversal using the Filtered-DiskANN algorithm - not before or after search.

### Creating an Index with Metadata

```typescript
import { DatabaseSync } from "@photostructure/sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);

// Create index with metadata columns
db.exec(`
  CREATE VIRTUAL TABLE photos USING diskann(
    dimension=512,
    metric=cosine,
    category TEXT,
    year INTEGER,
    score REAL
  )
`);
```

**Supported column types**: `TEXT`, `INTEGER`, `REAL`, `BLOB`

**Reserved names**: Cannot use `vector`, `distance`, `k`, or `rowid` as metadata column names

### Inserting Vectors with Metadata

```typescript
const embedding = new Float32Array(512); // Your vector embedding

db.prepare(
  "INSERT INTO photos(rowid, vector, category, year, score) VALUES (?, ?, ?, ?, ?)"
).run(1, embedding, "landscape", 2024, 0.95);

db.prepare(
  "INSERT INTO photos(rowid, vector, category, year, score) VALUES (?, ?, ?, ?, ?)"
).run(2, embedding, "portrait", 2023, 0.87);
```

### Searching with Metadata Filters

Metadata filters are evaluated **during beam search**, not as a post-filter. This ensures correct recall even with selective filters.

```typescript
const query = new Float32Array(512);

// Filter by category
const landscapes = db
  .prepare(
    `
  SELECT rowid, distance, category, year
  FROM photos
  WHERE vector MATCH ? AND k = 10 AND category = 'landscape'
`
  )
  .all(query);

// Multiple filters
const recent = db
  .prepare(
    `
  SELECT rowid, distance, category, year, score
  FROM photos
  WHERE vector MATCH ? AND k = 10
    AND category = 'landscape'
    AND year >= 2023
    AND score > 0.8
`
  )
  .all(query);

// Range filters
const filtered = db
  .prepare(
    `
  SELECT rowid, distance, category
  FROM photos
  WHERE vector MATCH ? AND k = 10 AND year BETWEEN 2020 AND 2024
`
  )
  .all(query);
```

**Supported filter operators**: `=`, `!=`, `<`, `<=`, `>`, `>=`, `BETWEEN`, `IN`

### TypeScript Helper Functions

```typescript
import { createDiskAnnIndex } from "@photostructure/sqlite-diskann";

// Create index with metadata columns
createDiskAnnIndex(db, "photos", {
  dimension: 512,
  metric: "cosine",
  metadataColumns: [
    { name: "category", type: "TEXT" },
    { name: "year", type: "INTEGER" },
    { name: "score", type: "REAL" },
  ],
});

// Insert using raw SQL for metadata
const vec = new Float32Array(512);
db.prepare("INSERT INTO photos(rowid, vector, category, year) VALUES (?, ?, ?, ?)").run(
  1,
  vec,
  "landscape",
  2024
);

// Search with filters (use raw SQL)
const results = db
  .prepare(
    `
  SELECT rowid, distance, category, year
  FROM photos
  WHERE vector MATCH ? AND k = 10 AND category = ?
`
  )
  .all(vec, "landscape");
```

## MATCH Operator Syntax

The `MATCH` operator triggers ANN search. It must be combined with the `k` parameter.

### Basic Search

```sql
SELECT rowid, distance
FROM embeddings
WHERE vector MATCH <vector_blob> AND k = <neighbor_count>
```

- `vector MATCH <blob>`: Triggers ANN search with the query vector (must be BLOB)
- `k = <number>`: Number of nearest neighbors to return
- Results are automatically sorted by distance (ascending)

### With LIMIT

```sql
-- LIMIT caps result rows, not search beam width
SELECT rowid, distance
FROM embeddings
WHERE vector MATCH ? AND k = 100
LIMIT 10  -- Returns closest 10 of the 100 candidates
```

**Note**: `k` controls the search beam width (quality), `LIMIT` controls result count.

### With Metadata Filters

```sql
-- Filters are evaluated DURING graph traversal (Filtered-DiskANN)
SELECT rowid, distance, category, year
FROM photos
WHERE vector MATCH ? AND k = 50 AND category = 'landscape' AND year > 2020
```

**How filtering works**:

1. Graph traversal visits all nodes (respecting graph edges as bridges)
2. Only matching nodes are added to the top-k results
3. Non-matching nodes are still traversed (to reach matching nodes elsewhere)
4. Returns up to k matching results

### Invalid Queries

```sql
-- ❌ Missing k parameter
SELECT rowid, distance FROM embeddings WHERE vector MATCH ?

-- ❌ k without MATCH
SELECT rowid, distance FROM embeddings WHERE k = 10

-- ❌ Wrong column type (vector must be BLOB, not TEXT)
SELECT rowid, distance FROM embeddings WHERE vector MATCH '[1.0, 2.0, ...]' AND k = 10
```

## Performance Tips

### Index Metadata Columns

For fast filtered search, create SQLite indexes on metadata columns you filter by:

```sql
-- Create index with metadata columns
CREATE VIRTUAL TABLE photos USING diskann(
  dimension=512, metric=cosine, category TEXT, year INTEGER
);

-- Add index on frequently filtered columns in the shadow table
-- Shadow table name pattern: {tableName}_attrs
CREATE INDEX idx_photos_category ON photos_attrs(category);
CREATE INDEX idx_photos_year ON photos_attrs(year);
CREATE INDEX idx_photos_combined ON photos_attrs(category, year);
```

**Why**: Metadata is stored in a shadow table named `{tableName}_attrs` (e.g., `photos_attrs` for a table named `photos`). SQLite indexes on this shadow table speed up the pre-filtering step before beam search.

**When to index**:

- ✅ Columns used in WHERE clauses (e.g., `category = 'landscape'`)
- ✅ High-cardinality columns (many unique values)
- ✅ Selective filters (< 50% of rows match)
- ❌ Low-cardinality columns (e.g., boolean flags)
- ❌ Columns rarely used in filters

### Tuning Search Parameters

```sql
-- Create index with tuned parameters
CREATE VIRTUAL TABLE embeddings USING diskann(
  dimension=512,
  metric=cosine,
  max_degree=64,              -- Graph connectivity (default: 64)
  build_search_list_size=100  -- Beam width during insert (default: 100)
);
```

- **`max_degree`**: Higher values improve recall but increase memory and index size
  - Default: 64
  - Range: 16-128
  - Recommendation: 64 for most use cases

- **`build_search_list_size`**: Higher values improve index quality but slow down inserts
  - Default: 100
  - Range: 50-200
  - Recommendation: 100 for balanced performance

### Vector Format

Use `Float32Array` for best performance:

```typescript
// ✅ Good - direct binary encoding
const vec = new Float32Array(512);
db.prepare("INSERT INTO embeddings(rowid, vector) VALUES (?, ?)").run(1, vec);

// ✅ Also good - automatic conversion
const vecArray = [0.1, 0.2, 0.3, ...];  // number[]
insertVector(db, "embeddings", 1, vecArray);  // Converts to Float32Array internally
```

### Batch Operations

Use transactions for bulk inserts:

```typescript
db.exec("BEGIN TRANSACTION");
const stmt = db.prepare("INSERT INTO embeddings(rowid, vector) VALUES (?, ?)");
for (let i = 0; i < 10000; i++) {
  stmt.run(i, vectors[i]);
}
db.exec("COMMIT");
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
