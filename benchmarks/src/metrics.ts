/**
 * Metrics calculations for benchmark analysis
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { mean, percentile, standardDeviation } from "./utils/stats.js";

/**
 * Compute recall@k
 *
 * Recall@k = |predicted ∩ ground_truth| / k
 *
 * @param predicted - IDs returned by search (in any order)
 * @param groundTruth - True k nearest neighbor IDs
 * @returns Recall value (0-1)
 */
export function computeRecallAtK(predicted: number[], groundTruth: number[]): number {
  if (groundTruth.length === 0) return 0;

  const gtSet = new Set(groundTruth);
  const intersection = predicted.filter((id) => gtSet.has(id)).length;

  return intersection / groundTruth.length;
}

/**
 * Statistics computed from latency measurements
 */
export interface LatencyStats {
  /** 50th percentile (median) latency (ms) */
  p50: number;
  /** 95th percentile latency (ms) */
  p95: number;
  /** 99th percentile latency (ms) */
  p99: number;
  /** Mean latency (ms) */
  mean: number;
  /** Standard deviation (ms) */
  std: number;
  /** Minimum latency (ms) */
  min: number;
  /** Maximum latency (ms) */
  max: number;
}

/**
 * Compute statistics from latency measurements
 *
 * @param latencies - Array of latency measurements (ms)
 * @returns Latency statistics
 */
export function computeStats(latencies: number[]): LatencyStats {
  if (latencies.length === 0) {
    return {
      p50: 0,
      p95: 0,
      p99: 0,
      mean: 0,
      std: 0,
      min: 0,
      max: 0,
    };
  }

  const sorted = [...latencies].sort((a, b) => a - b);

  return {
    p50: percentile(sorted, 0.5),
    p95: percentile(sorted, 0.95),
    p99: percentile(sorted, 0.99),
    mean: mean(latencies),
    std: standardDeviation(latencies),
    min: sorted[0],
    max: sorted[sorted.length - 1],
  };
}
