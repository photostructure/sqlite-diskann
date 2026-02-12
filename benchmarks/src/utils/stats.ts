/**
 * Statistical utilities for benchmark analysis
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * Calculate percentile from sorted array
 *
 * @param sorted - Sorted array of numbers
 * @param p - Percentile (0-1)
 * @returns Value at percentile p
 */
export function percentile(sorted: number[], p: number): number {
  if (sorted.length === 0) return 0;
  if (sorted.length === 1) return sorted[0];

  const index = (sorted.length - 1) * p;
  const lower = Math.floor(index);
  const upper = Math.ceil(index);
  const weight = index - lower;

  // False positive: lower and upper are computed from array length via Math.floor/ceil
  // eslint-disable-next-line security/detect-object-injection
  return sorted[lower] * (1 - weight) + sorted[upper] * weight;
}

/**
 * Calculate mean
 *
 * @param values - Array of numbers
 * @returns Mean value
 */
export function mean(values: number[]): number {
  if (values.length === 0) return 0;
  return values.reduce((a, b) => a + b, 0) / values.length;
}

/**
 * Calculate standard deviation
 *
 * @param values - Array of numbers
 * @returns Standard deviation
 */
export function standardDeviation(values: number[]): number {
  if (values.length === 0) return 0;
  const avg = mean(values);
  const squareDiffs = values.map((value) => (value - avg) ** 2);
  return Math.sqrt(mean(squareDiffs));
}
