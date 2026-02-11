# Usage Guide

Complete usage guide for @photostructure/sqlite-diskann

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

## C API (Advanced)

For direct C API usage, the lower-level functions are available:

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
