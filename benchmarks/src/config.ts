/**
 * Benchmark configuration and result types
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * Configuration for a benchmark run
 */
export interface BenchmarkConfig {
  /** Name of this benchmark configuration */
  name: string;

  /** Dataset configuration */
  dataset: {
    /** Path to binary dataset file */
    path: string;
    /** Optional: use subset of vectors (for testing) */
    sampleSize?: number;
  };

  /** Query configuration */
  queries: {
    /** Number of queries to run */
    count: number;
    /** Specific vector indices to use as queries (random if undefined) */
    indices?: number[];
    /** k values to test (e.g., [1, 10, 50, 100]) */
    k: number[];
  };

  /** Libraries to benchmark */
  libraries: Array<{
    /** Library name */
    name: "diskann" | "vec" | "usearch";
    /** Library-specific parameters */
    params?: Record<string, unknown>;
  }>;

  /** DiskANN-specific configuration */
  diskann?: {
    /** Maximum degree of graph nodes (e.g., [32, 64, 128]) */
    maxDegree: number[];
    /** Search list size during index construction (e.g., [100]) */
    buildSearchListSize: number[];
    /** Search beam width (e.g., [50, 100, 200]) */
    searchListSize: number[];
    /** Distance metric */
    metric: "cosine" | "euclidean" | "dot";
    /** Normalize vectors during insertion */
    normalizeVectors?: boolean;
  };

  /** USearch (HNSW) configuration. Pass 0 or omit to let USearch auto-detect. */
  usearch?: {
    /** HNSW connectivity parameter (M). 0 = auto-detect. */
    connectivity?: number;
    /** Expansion factor during index construction (ef_construction). 0 = auto-detect. */
    expansionAdd?: number;
    /** Expansion factor during search (ef). 0 = auto-detect, but always >= 2*max(k). */
    expansionSearch?: number;
    /** Distance metric */
    metric?: "cosine" | "euclidean" | "dot";
  };

  /** sqlite-vec-specific configuration */
  vec?: {
    /** Chunk size for vector scanning (optional) */
    chunkSize?: number;
    /** SQLite page size (optional) */
    pageSize?: number;
  };

  /** Metrics configuration */
  metrics: {
    /** Whether to compute recall@k */
    computeRecall: boolean;
    /** Path to ground truth file (if not specified, will be computed) */
    groundTruthPath?: string;
    /** Whether to measure memory usage */
    measureMemory: boolean;
    /** Number of warmup queries before timing */
    warmupQueries: number;
  };
}

/**
 * Search result statistics for a specific k value
 */
export interface SearchStats {
  /** k value (number of neighbors) */
  k: number;
  /** Total number of queries run */
  totalQueries: number;
  /** Total time for all queries (ms) */
  totalTime: number;
  /** 50th percentile latency (ms) */
  latencyP50: number;
  /** 95th percentile latency (ms) */
  latencyP95: number;
  /** 99th percentile latency (ms) */
  latencyP99: number;
  /** Queries per second */
  qps: number;
  /** Recall@k (if ground truth available) */
  recall?: number;
}

/**
 * Result from a benchmark run
 */
export interface BenchmarkResult {
  /** Configuration used for this benchmark */
  config: BenchmarkConfig;
  /** Library name */
  library: string;
  /** Parameters used for this run */
  params: Record<string, unknown>;

  /** Build metrics */
  buildTime: number; // ms
  indexSize: number; // bytes
  memoryUsed?: number; // bytes

  /** Search metrics for each k value */
  search: SearchStats[];
}

/**
 * Exported results with metadata
 */
export interface ExportedResults {
  /** Timestamp of benchmark run */
  timestamp: string;
  /** Package version */
  version: string;
  /** Platform information */
  platform: {
    node: string;
    platform: string;
    arch: string;
  };
  /** Benchmark results */
  results: BenchmarkResult[];
}
