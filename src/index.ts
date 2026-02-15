/**
 * @photostructure/sqlite-diskann
 *
 * SQLite extension for DiskANN approximate nearest neighbor vector search
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type {
  DatabaseLike,
  DiskAnnIndexOptions,
  NearestNeighborResult,
  SearchOptions,
} from "./types.js";

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
  DatabaseLike,
  DiskAnnIndexOptions,
  DistanceMetric,
  MetadataColumn,
  MetadataColumnType,
  NearestNeighborResult,
  // SearchOptions currently unused but exported for future API enhancements
  SearchOptions,
  StatementLike,
} from "./types.js";

/**
 * Get the path to the native DiskANN extension for the current platform.
 *
 * Returns the full file path including extension (e.g., `/path/to/diskann.so`).
 * If calling `db.loadExtension()` directly, strip the file extension first —
 * SQLite's `sqlite3_load_extension` auto-appends platform suffixes.
 * Or use {@link loadDiskAnnExtension} which handles this automatically.
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
 * @param db - Database instance (supports node:sqlite, better-sqlite3, @photostructure/sqlite)
 * @throws {Error} If extension fails to load
 *
 * @example
 * ```ts
 * import { DatabaseSync } from "@photostructure/sqlite"; // or node:sqlite, or better-sqlite3
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
export function loadDiskAnnExtension(db: DatabaseLike): void {
  const fullPath = getExtensionPath();

  // Strip the file extension (.so/.dylib/.dll) before passing to loadExtension.
  // SQLite's sqlite3_load_extension auto-appends platform suffixes, so passing
  // the extensionless path works consistently across all implementations
  // (node:sqlite, better-sqlite3, @photostructure/sqlite).
  const extPath = fullPath.replace(/\.(so|dylib|dll)$/, "");

  db.loadExtension(extPath);
}

/**
 * Helper to create DiskANN virtual table with proper SQL escaping
 *
 * @param db - Database instance (supports node:sqlite, better-sqlite3, @photostructure/sqlite)
 * @param tableName - Name for the virtual table
 * @param options - Index configuration
 *
 * @example
 * ```ts
 * // Basic index
 * createDiskAnnIndex(db, "clip_embeddings", {
 *   dimension: 512,
 *   metric: "cosine",
 *   maxDegree: 64
 * });
 *
 * // With metadata columns for filtered search
 * createDiskAnnIndex(db, "photos", {
 *   dimension: 512,
 *   metric: "cosine",
 *   metadataColumns: [
 *     { name: 'category', type: 'TEXT' },
 *     { name: 'year', type: 'INTEGER' }
 *   ]
 * });
 * ```
 */
export function createDiskAnnIndex(
  db: DatabaseLike,
  tableName: string,
  options: DiskAnnIndexOptions
): void {
  const {
    dimension,
    metric = "cosine",
    maxDegree = 32,
    buildSearchListSize = 100,
    normalizeVectors = false,
    metadataColumns = [],
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

  // Validate metadata columns
  const reservedNames = ["vector", "distance", "k", "rowid"];
  const seenNames = new Set<string>();
  for (const col of metadataColumns) {
    if (!isValidIdentifier(col.name)) {
      throw new Error(
        `Invalid metadata column name: ${col.name} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
      );
    }
    if (reservedNames.includes(col.name.toLowerCase())) {
      throw new Error(
        `Reserved column name: ${col.name} (cannot use vector, distance, k, or rowid)`
      );
    }
    if (seenNames.has(col.name.toLowerCase())) {
      throw new Error(`Duplicate metadata column name: ${col.name}`);
    }
    seenNames.add(col.name.toLowerCase());

    if (!["TEXT", "INTEGER", "REAL", "BLOB"].includes(col.type)) {
      throw new Error(
        `Invalid column type for ${col.name}: ${col.type} (must be TEXT, INTEGER, REAL, or BLOB)`
      );
    }
  }

  // Build CREATE VIRTUAL TABLE statement
  // tableName is validated above, safe to interpolate
  const params = [
    `dimension=${dimension}`,
    `metric=${metric}`,
    `max_degree=${maxDegree}`,
    `build_search_list_size=${buildSearchListSize}`,
    `normalize_vectors=${normalizeVectors ? 1 : 0}`,
  ];

  // Add metadata column definitions
  for (const col of metadataColumns) {
    params.push(`${col.name} ${col.type}`);
  }

  const sql = `CREATE VIRTUAL TABLE ${tableName} USING diskann(${params.join(", ")})`;

  db.exec(sql);
}

/**
 * Search for k nearest neighbors in a DiskANN index
 *
 * @param db - Database instance (supports node:sqlite, better-sqlite3, @photostructure/sqlite)
 * @param tableName - Name of the DiskANN virtual table
 * @param queryVector - Query vector as Float32Array or number[] (must match index dimension)
 * @param k - Number of neighbors to return (default: 10)
 * @returns Array of k nearest neighbors sorted by distance, including any metadata columns
 *
 * @example
 * ```ts
 * // Basic search
 * const vec = new Float32Array([0.1, 0.2, 0.3]);
 * const results = searchNearest(db, "embeddings", vec, 10);
 * results.forEach(({ rowid, distance }) => {
 *   console.log(`ID: ${rowid}, Distance: ${distance}`);
 * });
 *
 * // With metadata columns (use raw SQL for filtering)
 * const stmt = db.prepare(`
 *   SELECT rowid, distance, category, year
 *   FROM photos
 *   WHERE vector MATCH ? AND k = ? AND category = 'landscape'
 * `);
 * const filtered = stmt.all(vec, 10);
 * ```
 */
export function searchNearest(
  db: DatabaseLike,
  tableName: string,
  queryVector: Float32Array | number[],
  k = 10,
  options?: SearchOptions
): NearestNeighborResult[] {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  // Validate inputs
  if (!queryVector || queryVector.length === 0) {
    throw new Error("Query vector must be non-empty array or Float32Array");
  }
  if (!Number.isInteger(k) || k <= 0) {
    throw new Error(`Invalid k: ${k} (must be positive integer)`);
  }

  // Convert to Float32Array if needed
  const vecArray =
    queryVector instanceof Float32Array ? queryVector : new Float32Array(queryVector);

  // Build SQL with optional search_list_size constraint
  let sql = `
    SELECT rowid, distance
    FROM ${tableName}
    WHERE vector MATCH ? AND k = ?`;

  const params: unknown[] = [vecArray, k];

  // Add search_list_size constraint if specified
  if (options?.searchListSize !== undefined) {
    if (!Number.isInteger(options.searchListSize) || options.searchListSize <= 0) {
      throw new Error(
        `Invalid searchListSize: ${options.searchListSize} (must be positive integer)`
      );
    }
    sql += ` AND search_list_size = ?`;
    params.push(options.searchListSize);
  }

  // Execute search
  const stmt = db.prepare(sql);
  const results = stmt.all(...params) as NearestNeighborResult[];
  return results;
}

/**
 * Insert a vector into a DiskANN index
 *
 * For indexes with metadata columns, use raw SQL instead to specify column names:
 *
 * @param db - Database instance (supports node:sqlite, better-sqlite3, @photostructure/sqlite)
 * @param tableName - Name of the DiskANN virtual table
 * @param rowid - Unique row identifier for this vector
 * @param vector - Vector as Float32Array or number[] (must match index dimension)
 *
 * @example
 * ```ts
 * // Basic insert (no metadata)
 * const vec = new Float32Array([0.1, 0.2, 0.3]);
 * insertVector(db, "embeddings", 1, vec);
 *
 * // With metadata - use raw SQL to specify column names
 * db.prepare("INSERT INTO photos(rowid, vector, category, year) VALUES (?, ?, ?, ?)")
 *   .run(1, vec, 'landscape', 2024);
 * ```
 */
export function insertVector(
  db: DatabaseLike,
  tableName: string,
  rowid: number,
  vector: Float32Array | number[]
): void {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  if (
    !vector ||
    (!(vector instanceof Float32Array) && !Array.isArray(vector)) ||
    vector.length === 0
  ) {
    throw new Error("Vector must be non-empty array or Float32Array");
  }

  // Convert to Float32Array if needed
  const vecArray = vector instanceof Float32Array ? vector : new Float32Array(vector);

  // tableName is validated above, safe to interpolate
  const stmt = db.prepare(`INSERT INTO ${tableName}(rowid, vector) VALUES (?, ?)`);
  stmt.run(rowid, vecArray);
}

/**
 * Delete a vector from a DiskANN index
 *
 * @param db - Database instance (supports node:sqlite, better-sqlite3, @photostructure/sqlite)
 * @param tableName - Name of the DiskANN virtual table
 * @param rowid - Row ID of vector to delete
 *
 * @example
 * ```ts
 * deleteVector(db, "embeddings", 1);
 * ```
 */
export function deleteVector(db: DatabaseLike, tableName: string, rowid: number): void {
  // Validate table name to prevent SQL injection
  if (!isValidIdentifier(tableName)) {
    throw new Error(
      `Invalid table name: ${tableName} (must be alphanumeric/underscore, start with letter/underscore, max ${MAX_IDENTIFIER_LEN} chars)`
    );
  }

  // tableName is validated above, safe to interpolate
  const stmt = db.prepare(`DELETE FROM ${tableName} WHERE rowid = ?`);
  stmt.run(rowid);
}
