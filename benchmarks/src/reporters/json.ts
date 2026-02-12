/**
 * JSON export reporter
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { readFileSync, writeFileSync } from "node:fs";
import type { BenchmarkResult, ExportedResults } from "../config.js";

/**
 * Export benchmark results to JSON file
 *
 * @param results - Benchmark results
 * @param path - Output file path
 */
export function exportJSON(results: BenchmarkResult[], path: string): void {
  // Read package.json for version
  const packageJson = JSON.parse(
    readFileSync(new URL("../../package.json", import.meta.url), "utf-8")
  );

  const output: ExportedResults = {
    timestamp: new Date().toISOString(),
    version: packageJson.version,
    platform: {
      node: process.version,
      platform: process.platform,
      arch: process.arch,
    },
    results,
  };

  writeFileSync(path, JSON.stringify(output, null, 2));
  console.log(`\nResults exported to ${path}\n`);
}
