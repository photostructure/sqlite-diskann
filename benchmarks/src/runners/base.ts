/**
 * Base benchmark runner interface
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { BenchmarkConfig } from "../config.js";

/**
 * Search result from a benchmark runner
 */
export interface SearchResult {
  /** IDs of nearest neighbors */
  ids: number[];
  /** Distances to nearest neighbors */
  distances: number[];
  /** Query latency in milliseconds */
  latency: number;
}

/**
 * Build result from a benchmark runner
 */
export interface BuildResult {
  /** Build time in milliseconds */
  buildTime: number;
  /** Index size in bytes */
  indexSize: number;
}

/**
 * Abstract benchmark runner
 *
 * Each runner implementation handles a specific library (diskann or vec)
 */
export abstract class BenchmarkRunner {
  /** Library name */
  abstract readonly name: string;

  /**
   * Set up the runner with configuration
   *
   * @param config - Benchmark configuration
   */
  abstract setup(config: BenchmarkConfig): Promise<void>;

  /**
   * Build index from vectors
   *
   * @param vectors - Flat Float32Array (count * dim elements)
   * @param dim - Vector dimensionality
   * @returns Build metrics
   */
  abstract buildIndex(vectors: Float32Array, dim: number): Promise<BuildResult>;

  /**
   * Search for k nearest neighbors
   *
   * @param query - Query vector (dim elements)
   * @param k - Number of neighbors to return
   * @returns Search result with IDs, distances, and latency
   */
  abstract search(query: Float32Array, k: number): Promise<SearchResult>;

  /**
   * Clean up resources
   */
  abstract cleanup(): Promise<void>;
}
