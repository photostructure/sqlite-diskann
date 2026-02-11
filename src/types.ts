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
    changes: number;
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
 */
export interface DiskAnnIndexOptions {
  /**
   * Vector dimension (must be > 0)
   */
  dimension: number;

  /**
   * Distance metric
   * - "cosine": Cosine similarity (default for normalized embeddings)
   * - "euclidean": L2 distance
   * - "dot": Dot product (inner product)
   */
  metric?: DistanceMetric;

  /**
   * Maximum degree of graph nodes (default: 64)
   * Higher values improve recall but increase memory and index size
   */
  maxDegree?: number;

  /**
   * Search list size during index construction (default: 100)
   * Higher values improve index quality but slow down insertions
   */
  buildSearchListSize?: number;

  /**
   * Whether to normalize vectors during insertion (default: false)
   * Set to true for cosine similarity with non-normalized inputs
   */
  normalizeVectors?: boolean;

  /**
   * Optional metadata columns to store alongside vectors
   * Enables filtered search: WHERE vector MATCH ? AND k = 10 AND category = 'landscape'
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
 * Options for nearest neighbor search
 */
export interface SearchOptions {
  /**
   * Number of neighbors to return (default: 10)
   */
  k?: number;

  /**
   * Search beam width (default: 100)
   * Higher values improve recall but slow down search
   * Recommended range: [k, 10*k]
   */
  searchListSize?: number;
}
