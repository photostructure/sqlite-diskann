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
  const annResults = results.filter((r) => r.library !== "sqlite-vec");

  if (vecResult && annResults.length > 0) {
    const vecQps = vecResult.search[0].qps;
    console.log(`- sqlite-vec is always 100% recall (exact search)`);

    for (const annResult of annResults) {
      const annQps = annResult.search[0].qps;
      const speedup = annQps / vecQps;
      const recall = annResult.search[0].recall;

      console.log(
        `- ${annResult.library} is ${speedup.toFixed(1)}x faster than brute-force`
      );
      if (recall !== undefined) {
        console.log(
          `  Recall: ${(recall * 100).toFixed(1)}% (trades ${((1 - recall) * 100).toFixed(1)}% recall for ${speedup.toFixed(0)}x speedup)`
        );
      }
    }

    const bestAnn = annResults.reduce((best, r) =>
      r.search[0].qps > best.search[0].qps ? r : best
    );
    const bestSpeedup = bestAnn.search[0].qps / vecQps;

    if (bestSpeedup > 100) {
      console.log(`\nSignificant speedup achieved for this dataset size`);
    } else if (bestSpeedup > 10) {
      console.log(`\nGood speedup achieved for this dataset size`);
    } else {
      console.log(
        `\nFor small datasets, the overhead of approximate search may not be worth it`
      );
    }
  }

  console.log("");
}
