/**
 * Benchmark runner for USearch (HNSW)
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { statSync, unlinkSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Index, MetricKind, ScalarKind } from "usearch";
import type { BenchmarkConfig } from "../config.js";
import { Timer } from "../utils/timer.js";
import { BenchmarkRunner, type BuildResult, type SearchResult } from "./base.js";

/**
 * Map our metric names to USearch MetricKind
 */
function mapMetric(metric: "cosine" | "euclidean" | "dot"): MetricKind {
  switch (metric) {
    case "euclidean":
      return MetricKind.L2sq;
    case "cosine":
      return MetricKind.Cos;
    case "dot":
      return MetricKind.IP;
  }
}

/**
 * Benchmark runner for USearch (HNSW)
 *
 * Uses single-threaded operations for fair comparison with SQLite-based runners.
 */
export class USearchRunner extends BenchmarkRunner {
  readonly name = "usearch";

  private index!: Index;
  private config!: BenchmarkConfig;
  private tempPath: string | null = null;

  async setup(config: BenchmarkConfig): Promise<void> {
    this.config = config;
  }

  async buildIndex(vectors: Float32Array, dim: number): Promise<BuildResult> {
    const timer = new Timer();
    timer.start();

    const connectivity = this.config.usearch?.connectivity ?? 32;
    const expansionAdd = this.config.usearch?.expansionAdd ?? 512;
    const expansionSearch = this.config.usearch?.expansionSearch ?? 512;
    const metricName = this.config.usearch?.metric ?? "euclidean";
    const metric = mapMetric(metricName);

    // Ensure expansion_search >= 5*max(k) — high-D data needs wider beam
    const maxK = Math.max(...this.config.queries.k);
    const effectiveExpansionSearch = Math.max(expansionSearch, maxK * 5);

    this.index = new Index({
      dimensions: dim,
      metric,
      quantization: ScalarKind.F32,
      connectivity,
      expansion_add: expansionAdd,
      expansion_search: effectiveExpansionSearch,
      multi: false,
    });

    console.log(
      `  USearch params: M=${this.index.connectivity()}, ` +
        `ef_construction=${expansionAdd}, ` +
        `ef_search=${effectiveExpansionSearch}`
    );

    // Insert vectors one at a time with 0-based keys (single-threaded)
    const count = vectors.length / dim;
    for (let i = 0; i < count; i++) {
      const vec = vectors.slice(i * dim, (i + 1) * dim);
      this.index.add(BigInt(i), vec, 1);
    }

    const buildTime = timer.stop();

    // Measure index size by saving to temp file
    this.tempPath = join(tmpdir(), `usearch-bench-${Date.now()}.index`);
    this.index.save(this.tempPath);
    const indexSize = statSync(this.tempPath).size;

    return { buildTime, indexSize };
  }

  async search(query: Float32Array, k: number): Promise<SearchResult> {
    const timer = new Timer();
    timer.start();

    // Single-threaded search for fair comparison
    const results = this.index.search(query, k, 1);

    const latency = timer.stop();

    return {
      ids: Array.from(results.keys).map(Number),
      distances: Array.from(results.distances),
      latency,
    };
  }

  async cleanup(): Promise<void> {
    if (this.tempPath) {
      try {
        unlinkSync(this.tempPath);
      } catch {
        // ignore cleanup errors
      }
      this.tempPath = null;
    }
  }
}
