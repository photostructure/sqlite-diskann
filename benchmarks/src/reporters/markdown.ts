/**
 * Markdown report generator
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { BenchmarkResult } from "../config.js";

/**
 * Generate markdown report from benchmark results
 *
 * @param results - Benchmark results
 * @returns Markdown string
 */
export function generateMarkdownReport(results: BenchmarkResult[]): string {
  let md = "# Benchmark Results\n\n";
  md += `Generated: ${new Date().toISOString()}\n\n`;

  if (results.length === 0) {
    md += "No results to display.\n";
    return md;
  }

  // Summary table
  md += "## Summary\n\n";
  md +=
    "| Library | Dataset | k | Build (s) | QPS | p50 (ms) | p95 (ms) | Recall@k |\n";
  md +=
    "|---------|---------|---|-----------|-----|----------|----------|----------|\n";

  for (const result of results) {
    for (const search of result.search) {
      md += `| ${result.library} | ${result.config.name} | ${search.k} | ${(result.buildTime / 1000).toFixed(1)} | ${search.qps.toFixed(0)} | ${search.latencyP50.toFixed(2)} | ${search.latencyP95.toFixed(2)} | ${search.recall !== undefined ? `${(search.recall * 100).toFixed(1)}%` : "N/A"} |\n`;
    }
  }

  md += "\n## Key Findings\n\n";

  const vecResult = results.find((r) => r.library === "sqlite-vec");
  const diskannResult = results.find((r) => r.library === "sqlite-diskann");

  if (vecResult && diskannResult) {
    const speedup = diskannResult.search[0].qps / vecResult.search[0].qps;
    const diskannRecall = diskannResult.search[0].recall;

    md += `- **DiskANN is ${speedup.toFixed(1)}x faster** than brute-force sqlite-vec\n`;

    if (diskannRecall !== undefined) {
      md += `- DiskANN achieves **${(diskannRecall * 100).toFixed(1)}% recall** (trades ${((1 - diskannRecall) * 100).toFixed(1)}% accuracy for ${speedup.toFixed(0)}x speedup)\n`;
    }

    md += `- sqlite-vec provides **100% recall** (exact search, guaranteed correct results)\n`;
  }

  md += "\n## Performance Characteristics\n\n";
  md += "### sqlite-vec (Brute Force)\n\n";
  md += "- ✅ Always 100% recall (exact search)\n";
  md += "- ✅ Simple, no parameters to tune\n";
  md += "- ✅ Fast for small datasets (< 10k vectors)\n";
  md += "- ❌ Linear O(n) search time\n";
  md += "- ❌ Doesn't scale to large datasets\n\n";

  md += "### sqlite-diskann (Approximate)\n\n";
  md += "- ✅ Scales to millions of vectors\n";
  md += "- ✅ Sub-linear search time\n";
  md += "- ✅ Disk-based (doesn't require all data in RAM)\n";
  md += "- ❌ 95-99% recall (approximate, not exact)\n";
  md += "- ❌ More parameters to tune\n";
  md += "- ❌ Longer build time\n\n";

  return md;
}
