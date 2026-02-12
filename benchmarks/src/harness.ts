/**
 * Main benchmark orchestration
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { BenchmarkConfig, BenchmarkResult, SearchStats } from "./config.js";
import { loadDataset, randomSample } from "./dataset.js";
import { loadOrComputeGroundTruth } from "./ground-truth.js";
import { computeRecallAtK, computeStats } from "./metrics.js";
import type { BenchmarkRunner } from "./runners/base.js";
import { DiskAnnRunner } from "./runners/diskann-runner.js";
import { VecRunner } from "./runners/vec-runner.js";

/**
 * Create a runner for the specified library
 *
 * @param name - Library name
 * @returns Benchmark runner instance
 */
function createRunner(name: "diskann" | "vec"): BenchmarkRunner {
  switch (name) {
    case "diskann":
      return new DiskAnnRunner();
    case "vec":
      return new VecRunner();
    default:
      throw new Error(`Unknown library: ${name}`);
  }
}

/**
 * Run benchmark with the given configuration
 *
 * @param config - Benchmark configuration
 * @returns Array of benchmark results
 */
export async function runBenchmark(
  config: BenchmarkConfig
): Promise<BenchmarkResult[]> {
  const results: BenchmarkResult[] = [];

  console.log(`\n=== Loading dataset: ${config.dataset.path} ===\n`);

  // 1. Load dataset
  const { vectors, dim, count } = loadDataset(config.dataset.path);
  console.log(`Loaded ${count} vectors (${dim}d)\n`);

  // 2. Load or compute ground truth (if needed)
  let groundTruth: Awaited<ReturnType<typeof loadOrComputeGroundTruth>> | undefined;

  if (config.metrics.computeRecall) {
    const maxK = Math.max(...config.queries.k);
    const queryIndices =
      config.queries.indices ?? randomSample(count, config.queries.count, 42);

    console.log(
      `Computing ground truth for ${queryIndices.length} queries (k=${maxK})...`
    );

    groundTruth = await loadOrComputeGroundTruth(
      vectors,
      dim,
      queryIndices,
      maxK,
      config.metrics.groundTruthPath
    );

    console.log(`Ground truth ready\n`);
  }

  // 3. Select query vectors
  const queryIndices =
    config.queries.indices ?? randomSample(count, config.queries.count, 42);
  const queryVectors = queryIndices.map((idx) =>
    vectors.slice(idx * dim, (idx + 1) * dim)
  );

  // 4. Run benchmarks for each library
  for (const libConfig of config.libraries) {
    console.log(`\n=== Benchmarking ${libConfig.name} ===\n`);

    const runner = createRunner(libConfig.name);

    try {
      await runner.setup(config);

      // Build index
      console.log(`Building index...`);
      const { buildTime, indexSize } = await runner.buildIndex(vectors, dim);
      console.log(
        `  Build time: ${(buildTime / 1000).toFixed(1)}s, Index size: ${(indexSize / 1024 / 1024).toFixed(1)} MB\n`
      );

      // Warmup
      if (config.metrics.warmupQueries > 0) {
        console.log(`Warming up with ${config.metrics.warmupQueries} queries...`);
        for (let i = 0; i < config.metrics.warmupQueries; i++) {
          const queryVec = queryVectors[i % queryVectors.length];
          await runner.search(queryVec, config.queries.k[0]);
        }
        console.log(`Warmup complete\n`);
      }

      // Run searches for each k
      const searchStats: SearchStats[] = [];

      for (const k of config.queries.k) {
        console.log(`Running searches (k=${k})...`);

        const latencies: number[] = [];
        const recalls: number[] = [];

        for (let i = 0; i < queryVectors.length; i++) {
          // eslint-disable-next-line security/detect-object-injection
          const queryVec = queryVectors[i];

          const { ids, latency } = await runner.search(queryVec, k);
          latencies.push(latency);

          // Compute recall if ground truth available
          if (groundTruth) {
            // eslint-disable-next-line security/detect-object-injection
            const gtIds = groundTruth.neighbors[i].slice(0, k);
            const recall = computeRecallAtK(ids, gtIds);
            recalls.push(recall);

            // Debug: log first query results
            if (i === 0 && k === 10) {
              console.log(`\n[DEBUG] Query 0, k=${k}:`);
              console.log(`  Ground truth IDs:`, gtIds.slice(0, 10));
              console.log(`  DiskANN IDs:`, ids.slice(0, 10));
              console.log(`  Recall:`, recall);
            }
          }
        }

        // Compute statistics
        const stats = computeStats(latencies);
        const totalTime = latencies.reduce((a, b) => a + b, 0);
        const qps = (1000 * queryVectors.length) / totalTime;
        const avgRecall =
          recalls.length > 0
            ? recalls.reduce((a, b) => a + b) / recalls.length
            : undefined;

        searchStats.push({
          k,
          totalQueries: queryVectors.length,
          totalTime,
          latencyP50: stats.p50,
          latencyP95: stats.p95,
          latencyP99: stats.p99,
          qps,
          recall: avgRecall,
        });

        console.log(
          `  QPS: ${qps.toFixed(0)}, p50: ${stats.p50.toFixed(2)}ms, Recall: ${avgRecall ? (avgRecall * 100).toFixed(1) + "%" : "N/A"}\n`
        );
      }

      // Collect results
      results.push({
        config,
        library: runner.name,
        params: libConfig.params ?? {},
        buildTime,
        indexSize,
        search: searchStats,
      });
    } finally {
      await runner.cleanup();
    }
  }

  return results;
}
