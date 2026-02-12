/**
 * Benchmark runner for sqlite-vec
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { DatabaseSync } from "node:sqlite";
import { load as loadSqliteVec } from "sqlite-vec";
import type { BenchmarkConfig } from "../config.js";
import { Timer } from "../utils/timer.js";
import { BenchmarkRunner, type BuildResult, type SearchResult } from "./base.js";

/**
 * Benchmark runner for sqlite-vec
 */
export class VecRunner extends BenchmarkRunner {
  readonly name = "sqlite-vec";

  private db!: DatabaseSync;
  private tableName = "bench_vectors";

  // eslint-disable-next-line @typescript-eslint/no-unused-vars
  async setup(_config: BenchmarkConfig): Promise<void> {
    // Use in-memory database with extension loading enabled
    this.db = new DatabaseSync(":memory:", { allowExtension: true });
    this.db.enableLoadExtension(true);
    loadSqliteVec(this.db);
  }

  async buildIndex(vectors: Float32Array, dim: number): Promise<BuildResult> {
    const timer = new Timer();
    timer.start();

    // Create virtual table (no index build - brute force)
    this.db.exec(`
      CREATE VIRTUAL TABLE ${this.tableName} USING vec0(
        embedding float[${dim}]
      )
    `);

    // Insert vectors (let SQLite auto-generate rowid)
    const count = vectors.length / dim;
    const insertStmt = this.db.prepare(
      `INSERT INTO ${this.tableName}(embedding) VALUES (?)`
    );

    for (let i = 0; i < count; i++) {
      const vec = vectors.slice(i * dim, (i + 1) * dim);
      insertStmt.run(vec);
    }

    // Note: rowid will be auto-generated 1-based, so rowid-1 gives us the original index

    const buildTime = timer.stop();

    // Measure index size
    const sizeResult = this.db
      .prepare(
        "SELECT page_count * page_size as size FROM pragma_page_count(), pragma_page_size()"
      )
      .get() as { size: number };

    return {
      buildTime,
      indexSize: sizeResult.size,
    };
  }

  async search(query: Float32Array, k: number): Promise<SearchResult> {
    const timer = new Timer();
    timer.start();

    const results = this.db
      .prepare(
        `
        SELECT rowid, distance
        FROM ${this.tableName}
        WHERE embedding MATCH ?
          AND k = ?
        ORDER BY distance
      `
      )
      .all(query, k) as Array<{ rowid: number; distance: number }>;

    const latency = timer.stop();

    return {
      // Convert SQLite rowid (1-based) back to our 0-based indices
      ids: results.map((r) => r.rowid - 1),
      distances: results.map((r) => r.distance),
      latency,
    };
  }

  async cleanup(): Promise<void> {
    this.db.close();
  }
}
