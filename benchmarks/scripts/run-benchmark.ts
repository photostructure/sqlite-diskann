#!/usr/bin/env tsx
/**
 * Benchmark runner CLI
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { readFileSync } from "node:fs";
import type { BenchmarkConfig } from "../src/config.js";
import { runBenchmark } from "../src/harness.js";
import { printResults } from "../src/reporters/console.js";
import { exportJSON } from "../src/reporters/json.js";

async function main() {
  // Get profile path from command line or use default
  const profilePath = process.argv[2] ?? "profiles/quick.json";

  console.log(`\nLoading benchmark profile: ${profilePath}\n`);

  // Load configuration
  let config: BenchmarkConfig;
  try {
    const json = readFileSync(profilePath, "utf-8");
    config = JSON.parse(json);
  } catch (error) {
    console.error(`Failed to load profile: ${error}`);
    console.log("\nUsage: npm run bench [profile-path]");
    console.log("\nAvailable profiles:");
    console.log("  profiles/quick.json         - Quick smoke test (< 2 min)");
    console.log("  profiles/standard.json      - Standard benchmark (10-15 min)");
    console.log("  profiles/recall-sweep.json  - Recall vs speed sweep (15-20 min)");
    process.exit(1);
  }

  console.log(`Benchmark: ${config.name}\n`);

  // Run benchmark
  const results = await runBenchmark(config);

  // Print results to console
  printResults(results);

  // Export to JSON
  const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
  const outputPath = `results-${timestamp}.json`;
  exportJSON(results, outputPath);
}

main().catch((error) => {
  console.error("\nBenchmark failed:", error);
  process.exit(1);
});
