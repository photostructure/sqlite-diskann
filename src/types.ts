/**
 * Type definitions for sqlite-diskann
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * Minimal statement interface compatible with node:sqlite, better-sqlite3, and @photostructure/sqlite
 */
export interface StatementLike {
  /**
   * Execute statement and return info about changes
   */
  run(...params: unknown[]): {
    changes: number | bigint;
    lastInsertRowid: number | bigint;
  };

  /**
   * Execute query and return all results
   */
  all(...params: unknown[]): unknown[];
}

/**
 * Minimal database interface compatible with multiple SQLite implementations.
 *
 * Compatible libraries:
 * - node:sqlite DatabaseSync (Node 22+)
 * - better-sqlite3 Database
 * - @photostructure/sqlite DatabaseSync
 *
 * This interface defines only the methods actually used by sqlite-diskann.
 * Any SQLite library providing these methods can be used.
 *
 * @example
 * ```typescript
 * // Node 22+ built-in
 * import { DatabaseSync } from 'node:sqlite';
 * const db = new DatabaseSync(':memory:');
 *
 * // better-sqlite3
 * import Database from 'better-sqlite3';
 * const db = new Database(':memory:');
 *
 * // @photostructure/sqlite
 * import { DatabaseSync } from '@photostructure/sqlite';
 * const db = new DatabaseSync(':memory:');
 *
 * // All work with sqlite-diskann
 * loadDiskAnnExtension(db);
 * ```
 */
export interface DatabaseLike {
  /**
   * Load a SQLite extension from a file path
   *
   * @param path - Absolute path to the extension file (.so, .dylib, .dll)
   * @param entryPoint - Optional entry point function name (default: sqlite3_extension_init)
   */
  loadExtension(path: string, entryPoint?: string): void;

  /**
   * Execute one or more SQL statements without returning results
   * Used for DDL (CREATE TABLE, etc.) and DML without result rows
   *
   * @param sql - SQL statement(s) to execute
   */
  exec(sql: string): void;

  /**
   * Compile a SQL statement into a prepared statement
   *
   * @param sql - SQL statement with optional placeholders (?)
   * @returns Prepared statement object
   */
  prepare(sql: string): StatementLike;
}

/**
 * Distance metrics supported by DiskANN
 */
export type DistanceMetric = "cosine" | "euclidean" | "dot";

/**
 * Metadata column types supported by virtual table
 */
export type MetadataColumnType = "TEXT" | "INTEGER" | "REAL" | "BLOB";

/**
 * Metadata column definition for virtual table
 */
export interface MetadataColumn {
  /**
   * Column name (must be valid SQL identifier: alphanumeric/underscore, starts with letter/underscore)
   */
  name: string;

  /**
   * SQLite column type
   */
  type: MetadataColumnType;
}

/**
 * Configuration options for creating a DiskANN index
 *
 * @see {@link https://github.com/photostructure/sqlite-diskann/blob/main/PARAMETERS.md | Parameter Guide}
 *   for detailed explanations, mutability, and recommended values
 */
export interface DiskAnnIndexOptions {
  /**
   * Vector dimensionality (required)
   *
   * **🔒 IMMUTABLE** - Requires index rebuild to change
   *
   * Must match the size of vectors you'll insert. Common values:
   * - Small models (MiniLM): 384
   * - Medium models (BERT): 768
   * - Large models (OpenAI): 1536
   *
   * @example 768  // BERT embeddings
   */
  dimension: number;

  /**
   * Distance metric for similarity search
   *
   * **🔒 IMMUTABLE** - Requires index rebuild to change
   *
   * - `"euclidean"`: L2 distance (default, general use)
   * - `"cosine"`: Cosine similarity (recommended for text embeddings)
   * - `"dot"`: Dot product (for pre-normalized vectors)
   *
   * @default "euclidean"
   * @example "cosine"  // Recommended for text embeddings
   */
  metric?: DistanceMetric;

  /**
   * Maximum edges per node in the graph
   *
   * **🔒 IMMUTABLE** - Requires index rebuild to change
   *
   * Determines block size and connectivity. Recommended:
   * - Small datasets (<10k): 32
   * - Medium (10k-100k): 64
   * - Large (100k+): 96-128
   *
   * @default 64
   * @example 96  // Higher connectivity for large datasets
   */
  maxDegree?: number;

  /**
   * Beam width during INSERT (candidates explored when adding nodes)
   *
   * **⚠️ SEMI-MUTABLE** - Can be changed but requires rebuilding graph
   *
   * Major factor in build time (each insert = buildSearchListSize BLOB reads).
   * Recommended:
   * - Small datasets (<10k): 50-100
   * - Medium (10k-100k): 100-150
   * - Large (100k+): 150-200
   *
   * @default 200
   * @example 100  // Faster builds with acceptable recall
   */
  buildSearchListSize?: number;

  /**
   * Beam width during search (candidates explored per query)
   *
   * **✅ RUNTIME MUTABLE** - Can be overridden per-query via SearchOptions
   *
   * The effective beam width is automatically scaled to
   * `max(searchListSize, sqrt(indexSize))` to maintain recall as the
   * index grows. For most workloads, the default is sufficient.
   *
   * @default 100 (auto-scales with index size)
   * @see {@link SearchOptions.searchListSize} for per-query override
   */
  searchListSize?: number;

  /**
   * SQLite BLOB block size in bytes
   *
   * **🔒 IMMUTABLE** - Requires index rebuild to change
   *
   * Auto-calculated from dimension × maxDegree. Leave undefined for automatic.
   *
   * @default undefined (auto-calculate, recommended)
   * @example undefined
   */
  blockSize?: number;

  /**
   * Whether to normalize vectors during insertion
   *
   * Set to true for cosine similarity with non-normalized inputs.
   *
   * @default false
   */
  normalizeVectors?: boolean;

  /**
   * Optional metadata columns to store alongside vectors
   *
   * Enables filtered search:
   * ```sql
   * WHERE vector MATCH ? AND k = 10 AND category = 'landscape'
   * ```
   *
   * @example
   * ```typescript
   * {
   *   metadataColumns: [
   *     { name: 'category', type: 'TEXT' },
   *     { name: 'year', type: 'INTEGER' },
   *     { name: 'score', type: 'REAL' }
   *   ]
   * }
   * ```
   */
  metadataColumns?: MetadataColumn[];
}

/**
 * Result from a nearest neighbor search
 */
export interface NearestNeighborResult {
  /**
   * Row ID from the index (SQLite rowid)
   */
  rowid: number;

  /**
   * Distance to query vector (lower is closer for cosine/euclidean)
   * Interpretation depends on metric:
   * - cosine: 0 = identical, 2 = opposite
   * - euclidean: 0 = identical, higher = further
   * - dot: higher = more similar (NOT a distance)
   */
  distance: number;

  /**
   * Metadata columns (if defined in index)
   * Property names match column names from CREATE VIRTUAL TABLE
   */
  [key: string]: unknown;
}

/**
 * Options for controlling search behavior at query time
 *
 * All options can be specified per-query without rebuilding the index.
 */
export interface SearchOptions {
  /**
   * Number of neighbors to return
   *
   * @default 10
   * @example 10  // Return top 10 results
   */
  k?: number;

  /**
   * Override search beam width for this query
   *
   * **✅ RUNTIME MUTABLE** - Can be changed per-query without rebuilding
   *
   * Overrides the default searchListSize stored in index metadata.
   * The effective beam is `max(this_value, sqrt(index_size))`, so
   * setting a low value won't degrade recall on large indices.
   *
   * For most workloads, omit this — auto-scaling handles it.
   * Override only when you need faster queries and can accept lower recall.
   *
   * @default Auto-scaled: `max(stored_default, sqrt(index_size))`
   *
   * @example
   * ```typescript
   * // Use auto-scaled default (recommended)
   * searchNearest(db, 'vectors', query, 10)
   *
   * // Force wider beam for maximum recall
   * searchNearest(db, 'vectors', query, 10, { searchListSize: 500 })
   * ```
   */
  searchListSize?: number;
}
