/**
 * Parameter sweep utilities
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { BenchmarkConfig } from "./config.js";

/**
 * DiskANN parameter combination
 */
export interface DiskAnnParams {
  maxDegree: number;
  buildSearchListSize: number;
  searchListSize: number;
}

/**
 * Generate all combinations of DiskANN parameters for sweeping
 *
 * @param config - Benchmark configuration
 * @returns Array of parameter combinations
 */
export function generateDiskAnnParamCombinations(
  config: BenchmarkConfig
): DiskAnnParams[] {
  if (!config.diskann) {
    return [{ maxDegree: 64, buildSearchListSize: 100, searchListSize: 100 }];
  }

  const maxDegrees = config.diskann.maxDegree ?? [64];
  const buildSearchListSizes = config.diskann.buildSearchListSize ?? [100];
  const searchListSizes = config.diskann.searchListSize ?? [100];

  const combinations: DiskAnnParams[] = [];

  for (const maxDegree of maxDegrees) {
    for (const buildSearchListSize of buildSearchListSizes) {
      for (const searchListSize of searchListSizes) {
        combinations.push({ maxDegree, buildSearchListSize, searchListSize });
      }
    }
  }

  return combinations;
}

/**
 * Create a config for a specific parameter combination
 *
 * @param baseConfig - Base configuration
 * @param params - Parameter values to use
 * @returns Modified configuration
 */
export function createConfigForParams(
  baseConfig: BenchmarkConfig,
  params: DiskAnnParams
): BenchmarkConfig {
  return {
    ...baseConfig,
    diskann: baseConfig.diskann && {
      ...baseConfig.diskann,
      maxDegree: [params.maxDegree],
      buildSearchListSize: [params.buildSearchListSize],
      searchListSize: [params.searchListSize],
    },
  };
}

/**
 * Generate a label for a parameter combination
 *
 * @param params - Parameter values
 * @returns Human-readable label
 */
export function formatParamLabel(params: DiskAnnParams): string {
  return `maxDeg=${params.maxDegree}, buildL=${params.buildSearchListSize}, searchL=${params.searchListSize}`;
}
