/**
 * Type definitions for sqlite-diskann
 *
 * Copyright 2025 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * Distance metrics supported by DiskANN
 */
export type DistanceMetric = "cosine" | "euclidean" | "dot";

/**
 * Configuration options for creating a DiskANN index
 */
export interface DiskAnnIndexOptions {
  /**
   * Vector dimension (must be > 0)
   */
  dimension: number;

  /**
   * Distance metric
   * - "cosine": Cosine similarity (default for normalized embeddings)
   * - "euclidean": L2 distance
   * - "dot": Dot product (inner product)
   */
  metric?: DistanceMetric;

  /**
   * Maximum degree of graph nodes (default: 64)
   * Higher values improve recall but increase memory and index size
   */
  maxDegree?: number;

  /**
   * Search list size during index construction (default: 100)
   * Higher values improve index quality but slow down insertions
   */
  buildSearchListSize?: number;

  /**
   * Whether to normalize vectors during insertion (default: false)
   * Set to true for cosine similarity with non-normalized inputs
   */
  normalizeVectors?: boolean;
}

/**
 * Result from a nearest neighbor search
 */
export interface NearestNeighborResult {
  /**
   * Row ID from the index
   */
  id: number;

  /**
   * Distance to query vector (lower is closer for cosine/euclidean)
   * Interpretation depends on metric:
   * - cosine: 0 = identical, 2 = opposite
   * - euclidean: 0 = identical, higher = further
   * - dot: higher = more similar (NOT a distance)
   */
  distance: number;
}

/**
 * Options for nearest neighbor search
 */
export interface SearchOptions {
  /**
   * Number of neighbors to return (default: 10)
   */
  k?: number;

  /**
   * Search beam width (default: 100)
   * Higher values improve recall but slow down search
   * Recommended range: [k, 10*k]
   */
  searchListSize?: number;
}
