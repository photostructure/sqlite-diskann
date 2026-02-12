#!/usr/bin/env tsx
/**
 * Dataset preparation script
 *
 * Generates synthetic datasets with ground truth for benchmarking
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { generateRandomVectors, randomSample, saveDataset } from "../src/dataset.js";
import { computeGroundTruth } from "../src/ground-truth.js";

/** Dataset configurations */
const configs = [
  { name: "small-64d-10k", count: 10_000, dim: 64 },
  { name: "small-96d-10k", count: 10_000, dim: 96 },
  { name: "medium-256d-25k", count: 25_000, dim: 256 },
  { name: "medium-256d-100k", count: 100_000, dim: 256 },
  { name: "large-512d-100k", count: 100_000, dim: 512 },
];

/** Seed for reproducible datasets */
const SEED = 42;

/** Number of query vectors for ground truth */
const QUERY_COUNT = 200;

/** k value for ground truth */
const K = 100;

async function main() {
  console.log("\n=== Preparing Benchmark Datasets ===\n");

  // Create directories
  mkdirSync("datasets/synthetic", { recursive: true });
  mkdirSync("datasets/ground-truth", { recursive: true });

  for (const config of configs) {
    const datasetPath = `datasets/synthetic/${config.name}.bin`;
    const gtPath = `datasets/ground-truth/${config.name}.json`;

    // Skip if both dataset and ground truth already exist
    if (existsSync(datasetPath) && existsSync(gtPath)) {
      console.log(`\n✓ ${config.name} already exists, skipping`);
      continue;
    }

    console.log(
      `\nGenerating ${config.name} (${config.count} vectors × ${config.dim}d)...`
    );

    // Generate vectors
    const vectors = generateRandomVectors(config.count, config.dim, SEED);
    saveDataset(datasetPath, vectors, config.dim);

    const sizeKB = (vectors.length * 4) / 1024;
    const sizeMB = sizeKB / 1024;
    console.log(`  Saved to ${datasetPath} (${sizeMB.toFixed(1)} MB)`);

    // Generate ground truth (using L2/Euclidean distance - sqlite-vec default)
    // IMPORTANT: Use same seed as harness (42) for query sampling
    console.log(`  Computing ground truth (${QUERY_COUNT} queries, k=${K})...`);
    const queryIndices = randomSample(config.count, QUERY_COUNT, SEED);
    const groundTruth = await computeGroundTruth(vectors, config.dim, queryIndices, K);

    writeFileSync(gtPath, JSON.stringify(groundTruth, null, 2));
    console.log(`  Ground truth saved to ${gtPath}`);
  }

  console.log("\n\n✅ All datasets prepared successfully!\n");
  console.log("You can now run benchmarks:");
  console.log("  npm run bench:quick      - Quick smoke test (< 2 min)");
  console.log(
    "  npm run bench:medium     - Medium benchmark with 25k vectors (~5 min)"
  );
  console.log("  npm run bench:standard   - Standard benchmark (10-15 min)");
  console.log("  npm run bench:recall     - Recall vs speed sweep (15-20 min)\n");
}

main().catch((error) => {
  console.error("Error preparing datasets:", error);
  process.exit(1);
});
