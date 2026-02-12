/**
 * Benchmark runner for sqlite-diskann
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { DatabaseSync } from "node:sqlite";
import {
  createDiskAnnIndex,
  insertVector,
  loadDiskAnnExtension,
  searchNearest,
} from "../../../src/index.js";
import type { BenchmarkConfig } from "../config.js";
import { Timer } from "../utils/timer.js";
import { BenchmarkRunner, type BuildResult, type SearchResult } from "./base.js";

/**
 * Benchmark runner for sqlite-diskann
 */
export class DiskAnnRunner extends BenchmarkRunner {
  readonly name = "sqlite-diskann";

  private db!: DatabaseSync;
  private tableName = "bench_vectors";
  private config!: BenchmarkConfig;
  private searchListSize = 100;

  async setup(config: BenchmarkConfig): Promise<void> {
    this.config = config;

    // Use in-memory database with extension loading enabled
    this.db = new DatabaseSync(":memory:", { allowExtension: true });
    this.db.enableLoadExtension(true);
    loadDiskAnnExtension(this.db);

    // Set search list size from config
    if (config.diskann?.searchListSize?.[0]) {
      this.searchListSize = config.diskann.searchListSize[0];
    }
  }

  async buildIndex(vectors: Float32Array, dim: number): Promise<BuildResult> {
    const timer = new Timer();
    timer.start();

    // Get parameters from config
    const maxDegree = this.config.diskann?.maxDegree?.[0] ?? 64;
    const buildSearchListSize = this.config.diskann?.buildSearchListSize?.[0] ?? 100;
    const metric = this.config.diskann?.metric ?? "cosine";
    const normalizeVectors = this.config.diskann?.normalizeVectors ?? false;

    // Create index
    createDiskAnnIndex(this.db, this.tableName, {
      dimension: dim,
      metric,
      maxDegree,
      buildSearchListSize,
      normalizeVectors,
    });

    // Insert vectors in a single transaction for performance
    const count = vectors.length / dim;
    this.db.exec("BEGIN");
    try {
      for (let i = 0; i < count; i++) {
        const vec = Array.from(vectors.slice(i * dim, (i + 1) * dim));
        insertVector(this.db, this.tableName, i, vec);
      }
      this.db.exec("COMMIT");
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }

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

    const results = searchNearest(this.db, this.tableName, Array.from(query), k, {
      searchListSize: this.searchListSize,
    });

    const latency = timer.stop();

    return {
      ids: results.map((r) => r.rowid),
      distances: results.map((r) => r.distance),
      latency,
    };
  }

  async cleanup(): Promise<void> {
    this.db.close();
  }
}
