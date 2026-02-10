/**
 * @photostructure/sqlite-diskann
 *
 * SQLite extension for DiskANN approximate nearest neighbor vector search
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { DatabaseSyncInstance } from "@photostructure/sqlite";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { DiskAnnIndexOptions, NearestNeighborResult } from "./types.js";

// Type alias for convenience
type DatabaseSync = DatabaseSyncInstance;

/**
 * Maximum allowed length for SQL identifiers (table names, column names)
 * Must match C layer MAX_IDENTIFIER_LEN
 */
const MAX_IDENTIFIER_LEN = 64;

/**
 * Validate SQL identifier to prevent injection attacks
 * Must match C layer validate_identifier logic
 *
 * Rules:
 * - First character: letter or underscore
 * - Subsequent characters: letter, digit, or underscore
 * - Maximum length: 64 characters
 *
 * @param name - Identifier to validate
 * @returns true if valid, false otherwise
 */
function isValidIdentifier(name: string): boolean {
  if (!name || name.length === 0 || name.length > MAX_IDENTIFIER_LEN) {
    return false;
  }
  // Must start with letter or underscore, then alphanumeric or underscore
  return /^[a-z_]\w*$/i.test(name);
}

// Re-export types for convenience
export type {
  DiskAnnIndexOptions,
  DistanceMetric,
  NearestNeighborResult,
  // SearchOptions currently unused but exported for future API enhancements
  SearchOptions,
} from "./types.js";

/**
 * Get the path to the native DiskANN extension for the current platform
 */
export function getExtensionPath(): string {
  const platform = process.platform;
  const arch = process.arch;
  const ext = platform === "win32" ? "dll" : platform === "darwin" ? "dylib" : "so";

  const __filename = fileURLToPath(import.meta.url);
  const __dirname = dirname(__filename);

  // If we're in dist/, go up to package root
  const packageRoot = __dirname.endsWith("/dist")
    ? join(__dirname, "..")
    : join(__dirname, "..");

  // Map Node.js platform/arch to our prebuild directory names
  const platformArch = `${platform}-${arch}`;

  // In production (installed from npm), binaries are in prebuilds/{platform}-{arch}/
  // During development, they're in build/
  const productionPath = join(packageRoot, "prebuilds", platformArch, `diskann.${ext}`);
  const devPath = join(packageRoot, "build", `diskann.${ext}`);

  // Check if production prebuild exists, otherwise fall back to dev build
  if (existsSync(productionPath)) {
    return productionPath;
  }

  return devPath;
}

/**
 * Load the DiskANN extension into a SQLite database
 *
 * @param db - DatabaseSync instance from @photostructure/sqlite
 * @throws {Error} If extension fails to load
 *
 * @example
 * ```ts
 * import { DatabaseSync } from "@photostructure/sqlite";
 * import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";
 *
 * const db = new DatabaseSync(":memory:");
 * loadDiskAnnExtension(db);
 *
 * // Now you can use DiskANN functions
 * db.exec(`
 *   CREATE VIRTUAL TABLE embeddings USING diskann(
 *     dimension=512,
 *     metric=cosine
 *   )
 * `);
 * ```
 */
export function loadDiskAnnExtension(db: DatabaseSync): void {
  const extPath = getExtensionPath();
  db.loadExtension(extPath);
}

/**
 * Helper to create DiskANN virtual table with proper SQL escaping
 *
 * @param db - DatabaseSync instance
 * @param tableName - Name for the virtual table
 * @param options - Index configuration
 *
 * @example
 * ```ts
 * createDiskAnnIndex(db, "clip_embeddings", {
 *   dimension: 512,
 *   metric: "cosine",
 *   maxDegree: 64
 * });
 * ```
 */
export function createDiskAnnIndex(
  db: DatabaseSync,
  tableName: string,
  options: DiskAnnIndexOptions
): void {
  const {
    dimension,
    metric = "cosine",
    maxDegree = 64,
    buildSearchListSize = 100,
    normalizeVectors = false,
  } = options;

  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  // Validate inputs
  if (!Number.isInteger(dimension) || dimension <= 0) {
    throw new Error(`Invalid dimension: ${dimension} (must be positive integer)`);
  }
  if (!["cosine", "euclidean", "dot"].includes(metric)) {
    throw new Error(`Invalid metric: ${metric} (must be cosine, euclidean, or dot)`);
  }
  if (!Number.isInteger(maxDegree) || maxDegree <= 0) {
    throw new Error(`Invalid maxDegree: ${maxDegree} (must be positive integer)`);
  }

  // Build CREATE VIRTUAL TABLE statement
  // tableName is validated above, safe to interpolate
  const sql = `
    CREATE VIRTUAL TABLE ${tableName} USING diskann(
      dimension=${dimension},
      metric=${metric},
      max_degree=${maxDegree},
      build_search_list_size=${buildSearchListSize},
      normalize_vectors=${normalizeVectors ? 1 : 0}
    )
  `.trim();

  db.exec(sql);
}

/**
 * Search for k nearest neighbors in a DiskANN index
 *
 * @param db - DatabaseSync instance
 * @param tableName - Name of the DiskANN virtual table
 * @param queryVector - Query vector (must match index dimension)
 * @param k - Number of neighbors to return (default: 10)
 * @param searchListSize - Search beam width (default: 100, higher = better recall but slower)
 * @returns Array of k nearest neighbors sorted by distance
 *
 * @example
 * ```ts
 * const results = searchNearest(db, "embeddings", [0.1, 0.2, ...], 10);
 * console.log(`Found ${results.length} neighbors`);
 * results.forEach(({ id, distance }) => {
 *   console.log(`ID: ${id}, Distance: ${distance}`);
 * });
 * ```
 */
export function searchNearest(
  db: DatabaseSync,
  tableName: string,
  queryVector: number[],
  k = 10,
  searchListSize = 100
): NearestNeighborResult[] {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  // Validate inputs
  if (!Array.isArray(queryVector) || queryVector.length === 0) {
    throw new Error("Query vector must be non-empty array");
  }
  if (!Number.isInteger(k) || k <= 0) {
    throw new Error(`Invalid k: ${k} (must be positive integer)`);
  }

  // Convert vector to JSON for SQLite
  const vectorJson = JSON.stringify(queryVector);

  // Execute search query
  // Note: Actual SQL syntax depends on final C implementation
  // This is a placeholder for the expected interface
  // tableName is validated above, safe to interpolate
  const stmt = db.prepare(`
    SELECT id, distance
    FROM ${tableName}
    WHERE diskann_search(vector, ?, ?, ?)
    ORDER BY distance ASC
    LIMIT ?
  `);

  const results = stmt.all(vectorJson, k, searchListSize, k) as Array<{
    id: number;
    distance: number;
  }>;

  return results.map((row) => ({
    id: row.id,
    distance: row.distance,
  }));
}

/**
 * Insert a vector into a DiskANN index
 *
 * @param db - DatabaseSync instance
 * @param tableName - Name of the DiskANN virtual table
 * @param id - Unique identifier for this vector
 * @param vector - Vector to insert (must match index dimension)
 *
 * @example
 * ```ts
 * insertVector(db, "embeddings", 1, [0.1, 0.2, 0.3, ...]);
 * ```
 */
export function insertVector(
  db: DatabaseSync,
  tableName: string,
  id: number,
  vector: number[]
): void {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  if (!Array.isArray(vector) || vector.length === 0) {
    throw new Error("Vector must be non-empty array");
  }

  const vectorJson = JSON.stringify(vector);
  // tableName is validated above, safe to interpolate
  const stmt = db.prepare(`INSERT INTO ${tableName}(id, vector) VALUES (?, ?)`);
  stmt.run(id, vectorJson);
}

/**
 * Delete a vector from a DiskANN index
 *
 * @param db - DatabaseSync instance
 * @param tableName - Name of the DiskANN virtual table
 * @param id - ID of vector to delete
 */
export function deleteVector(db: DatabaseSync, tableName: string, id: number): void {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  // tableName is validated above, safe to interpolate
  const stmt = db.prepare(`DELETE FROM ${tableName} WHERE id = ?`);
  stmt.run(id);
}
