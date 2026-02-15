/**
 * Ground truth computation using brute-force exact search
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { load as loadSqliteVec } from "@photostructure/sqlite-vec";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { DatabaseSync } from "node:sqlite";

/**
 * Ground truth nearest neighbors
 */
export interface GroundTruth {
  /** Query vector indices */
  queries: number[];
  /** For each query: k nearest neighbor indices */
  neighbors: number[][];
  /** For each query: k distances */
  distances: number[][];
}

/**
 * Load ground truth from JSON file if exists, otherwise compute it
 *
 * @param vectors - All vectors in flat Float32Array
 * @param dim - Vector dimensionality
 * @param queryIndices - Indices of query vectors
 * @param k - Number of nearest neighbors
 * @param metric - Distance metric
 * @param cachePath - Path to cache file (optional)
 * @returns Ground truth object
 */
export async function loadOrComputeGroundTruth(
  vectors: Float32Array,
  dim: number,
  queryIndices: number[],
  k: number,
  cachePath?: string
): Promise<GroundTruth> {
  // Try to load from cache
  if (cachePath && existsSync(cachePath)) {
    try {
      const json = readFileSync(cachePath, "utf-8");
      const cached = JSON.parse(json) as GroundTruth;

      // Validate cache matches current parameters
      const queriesMatch =
        cached.queries.length === queryIndices.length &&
        cached.queries.every((q, i) => q === queryIndices[i]);
      const cachedK = cached.neighbors[0]?.length ?? 0;
      const kSufficient = cachedK >= k;

      if (queriesMatch && kSufficient) {
        return cached;
      }

      console.warn(
        `Ground truth cache stale (queries: ${cached.queries.length} vs ${queryIndices.length}, ` +
          `cached k: ${cachedK} vs requested k: ${k}), recomputing...`
      );
    } catch (error) {
      console.warn(`Failed to load ground truth from ${cachePath}:`, error);
      // Fall through to compute
    }
  }

  // Compute ground truth (always uses L2 distance via sqlite-vec)
  const groundTruth = await computeGroundTruth(vectors, dim, queryIndices, k);

  // Save to cache only if no existing cache has more data
  if (cachePath) {
    let shouldSave = true;
    if (existsSync(cachePath)) {
      try {
        const existing = JSON.parse(readFileSync(cachePath, "utf-8")) as GroundTruth;
        const existingK = existing.neighbors[0]?.length ?? 0;
        // Don't overwrite a cache with more queries or higher k
        if (existing.queries.length > queryIndices.length || existingK > k) {
          shouldSave = false;
        }
      } catch {
        // If we can't read existing, overwrite is fine
      }
    }
    if (shouldSave) {
      try {
        writeFileSync(cachePath, JSON.stringify(groundTruth, null, 2));
      } catch (error) {
        console.warn(`Failed to save ground truth to ${cachePath}:`, error);
      }
    }
  }

  return groundTruth;
}

/**
 * Compute ground truth using sqlite-vec brute-force search
 *
 * @param vectors - All vectors in flat Float32Array
 * @param dim - Vector dimensionality
 * @param queryIndices - Indices of query vectors
 * @param k - Number of nearest neighbors
 * @returns Ground truth object
 *
 * Note: metric parameter is currently not used; sqlite-vec defaults to L2 distance
 */
export async function computeGroundTruth(
  vectors: Float32Array,
  dim: number,
  queryIndices: number[],
  k: number
): Promise<GroundTruth> {
  const count = vectors.length / dim;

  // Create in-memory database with extension loading enabled
  const db = new DatabaseSync(":memory:", { allowExtension: true });
  // Enable extension loading (requires casting to access internal API)
  (db as { enableLoadExtension: (enable: boolean) => void }).enableLoadExtension(true);
  loadSqliteVec(db);

  // Create virtual table
  db.exec(`CREATE VIRTUAL TABLE vec USING vec0(embedding float[${dim}])`);

  // Insert all vectors (let SQLite auto-generate rowid)
  const insertStmt = db.prepare(`INSERT INTO vec(embedding) VALUES (?)`);
  for (let i = 0; i < count; i++) {
    const vec = vectors.slice(i * dim, (i + 1) * dim);
    insertStmt.run(vec);
  }

  // Note: rowid will be auto-generated 1-based, so rowid-1 gives us the original index

  // Search for each query
  const groundTruth: GroundTruth = {
    queries: queryIndices,
    neighbors: [],
    distances: [],
  };

  const searchStmt = db.prepare(`
    SELECT rowid, distance
    FROM vec
    WHERE embedding MATCH ?
      AND k = ?
    ORDER BY distance
  `);

  for (const queryIdx of queryIndices) {
    const queryVec = vectors.slice(queryIdx * dim, (queryIdx + 1) * dim);
    const results = searchStmt.all(queryVec, k) as Array<{
      rowid: number;
      distance: number;
    }>;

    // Convert SQLite rowid (1-based) back to our 0-based indices
    groundTruth.neighbors.push(results.map((r) => r.rowid - 1));
    groundTruth.distances.push(results.map((r) => r.distance));
  }

  // Close database
  db.close();

  return groundTruth;
}
