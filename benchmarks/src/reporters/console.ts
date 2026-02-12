/**
 * Console table reporter
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import Table from "cli-table3";
import type { BenchmarkResult } from "../config.js";

/**
 * Print benchmark results to console as formatted tables
 *
 * @param results - Benchmark results to display
 */
export function printResults(results: BenchmarkResult[]): void {
  if (results.length === 0) {
    console.log("\nNo results to display\n");
    return;
  }

  console.log("\n\n=== Benchmark Results ===\n");

  // Group by dataset and k
  const grouped: Map<string, BenchmarkResult[]> = new Map();

  for (const result of results) {
    for (const search of result.search) {
      const key = `${result.config.name}-k${search.k}`;
      const group = grouped.get(key);
      if (group) {
        group.push({
          ...result,
          search: [search],
        });
      } else {
        grouped.set(key, [
          {
            ...result,
            search: [search],
          },
        ]);
      }
    }
  }

  // Print table for each group
  for (const [key, group] of grouped.entries()) {
    const table = new Table({
      head: [
        "Library",
        "Build (s)",
        "Index (MB)",
        "QPS",
        "p50 (ms)",
        "p95 (ms)",
        "p99 (ms)",
        "Recall@k",
      ],
      colWidths: [18, 12, 12, 10, 10, 10, 10, 12],
      style: {
        head: ["cyan"],
        border: ["gray"],
      },
    });

    // Sort by QPS descending (fastest first)
    const sorted = group.sort((a, b) => b.search[0].qps - a.search[0].qps);

    for (const result of sorted) {
      const s = result.search[0];
      table.push([
        result.library,
        (result.buildTime / 1000).toFixed(1),
        (result.indexSize / 1024 / 1024).toFixed(1),
        s.qps.toFixed(0),
        s.latencyP50.toFixed(2),
        s.latencyP95.toFixed(2),
        s.latencyP99.toFixed(2),
        s.recall !== undefined ? `${(s.recall * 100).toFixed(1)}%` : "N/A",
      ]);
    }

    console.log(`${key}\n`);
    console.log(table.toString());
    console.log("");
  }

  // Key insights
  console.log("=== Key Insights ===\n");

  const vecResult = results.find((r) => r.library === "sqlite-vec");
  const diskannResult = results.find((r) => r.library === "sqlite-diskann");

  if (vecResult && diskannResult) {
    const vecQps = vecResult.search[0].qps;
    const diskannQps = diskannResult.search[0].qps;
    const speedup = diskannQps / vecQps;
    const diskannRecall = diskannResult.search[0].recall;

    console.log(`- DiskANN is ${speedup.toFixed(1)}x faster than brute-force`);

    if (diskannRecall !== undefined) {
      console.log(
        `- DiskANN recall: ${(diskannRecall * 100).toFixed(1)}% (trades ${((1 - diskannRecall) * 100).toFixed(1)}% recall for ${speedup.toFixed(0)}x speedup)`
      );
    }

    console.log(`- sqlite-vec is always 100% recall (exact search)`);

    if (speedup > 100) {
      console.log(`\n✨ DiskANN provides significant speedup for this dataset size`);
    } else if (speedup > 10) {
      console.log(`\n✓ DiskANN provides good speedup for this dataset size`);
    } else {
      console.log(
        `\nℹ For small datasets, the overhead of approximate search may not be worth it`
      );
    }
  }

  console.log("");
}
