# sqlite-diskann

> **This project is archived.** We extracted [libSQL's DiskANN implementation](https://github.com/tursodatabase/libsql) into a standalone SQLite extension and spent a week optimizing it, but the results weren't competitive. DiskANN assumes cheap memory-mapped graph traversal; SQLite's BLOB I/O adds substantial overhead per hop, making build times and index sizes impractical — even on clustered (GMM) vectors designed to favor graph indexes.
>
> **Use [@photostructure/sqlite-vec](https://github.com/photostructure/sqlite-vec) instead.** For datasets under 100k, brute-force search builds nearly instantly with perfect recall. For larger datasets, [USearch](https://github.com/unum-cloud/usearch) provides 5x the QPS with 99.6% recall.
>
> The code and [experiments](./experiments/) are preserved here as a reference. See [What went wrong](#what-went-wrong) below for the full story.

---

SQLite extension for DiskANN approximate nearest neighbor vector search.

## Database Compatibility

This package works with multiple SQLite library implementations through duck typing:

| Library                    | Availability           | Notes                                    |
| -------------------------- | ---------------------- | ---------------------------------------- |
| **@photostructure/sqlite** | npm package            | ✅ Stable, 100% `node:sqlite` compatible |
| **better-sqlite3**         | npm package            | ✅ Mature, stable, widely used           |
| **node:sqlite**            | Node.js 22.5+ built-in | ⚠️ Experimental (see note below)         |

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
# ⚠️ Experimental — Node 22.5–22.12 requires --experimental-sqlite flag
#    Node >= 22.13 / >= 23.4 no longer requires the flag
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

## What went wrong

We extracted libSQL's DiskANN into a standalone SQLite extension (Feb 2026). The code worked, but benchmarks told a different story.

**Recall collapsed at scale.** At 10k vectors, recall was 97%. At 100k, it dropped to near zero. The default 4KB BLOB block size only fits 2–3 edges per node for 256-dimensional vectors. The graph fragmented into disconnected components and search couldn't find its way.

**Fixing recall created new problems.** Auto-calculated 40KB blocks restored recall to 98%, but meant 7.5 GB indexes for 100k vectors and build times over an hour.

**We tried to optimize.** Persistent BLOB caching with refcounting (37% faster builds), batch insert API with amortized SAVEPOINT overhead, lazy back-edge deferral, and extensive parameter sweeps across `max_neighbors` and `search_list_size`. Each helped incrementally, but none addressed the fundamental issue.

**The problem is architectural.** DiskANN is designed for direct memory access: mmap the graph and traverse it cheaply. SQLite's BLOB I/O adds a read per graph hop. Each insert touches ~200 nodes, so at 100k inserts the cumulative I/O is enormous. No amount of caching fixes that mismatch.

**Clustered data didn't help.** We tested with GMM vectors (40 clusters, realistic for CLIP/FaceNet embeddings) hoping cluster structure would favor the graph index. It made things worse — recall dropped from 97% to 17% at 10k/512D. USearch (HNSW) handles the same data with 99.6% recall.

| Metric                 | sqlite-diskann | sqlite-vec | USearch (HNSW) |
| ---------------------- | -------------- | ---------- | -------------- |
| Build time (10k, 512D) | 147s           | 0.1s       | 0.3s           |
| Index size             | 747 MB         | 20 MB      | 22 MB          |
| QPS                    | 480            | 237        | 1,188          |
| Recall@10              | 17%            | 100%       | 99.6%          |

The full optimization journey is documented in [`_todo/`](./_todo/), [`_done/`](./_done/), and [`experiments/`](./experiments/).

**Are we wrong?** We'd genuinely like to know if there's a fundamental mistake in our code or our approach to testing. If you see something we missed, please email [this repo name]`@photostructure.com` and we'll take a look.

## License

MIT License

Derived from libSQL's DiskANN implementation:

- Copyright 2024 the libSQL authors
- Copyright 2026 PhotoStructure Inc.

## Links

- [DiskANN Paper (Microsoft Research)](https://proceedings.neurips.cc/paper/2019/file/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Paper.pdf)
- [libSQL DiskANN Implementation](https://github.com/tursodatabase/libsql)
- [Turso Blog: DiskANN in libSQL](https://turso.tech/blog/approximate-nearest-neighbor-search-with-diskann-in-libsql)
