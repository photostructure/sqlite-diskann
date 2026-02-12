/**
 * Dataset generation and binary I/O
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import { readFileSync, writeFileSync } from "node:fs";
import { createSeededRandom } from "./utils/vector-gen.js";

/** Magic header for dataset files */
const MAGIC = "VECDATA\0";

/**
 * Dataset structure
 */
export interface Dataset {
  /** All vectors in a flat Float32Array */
  vectors: Float32Array;
  /** Vector dimensionality */
  dim: number;
  /** Number of vectors */
  count: number;
}

/**
 * Save dataset to binary file
 *
 * Binary format:
 * [magic: 8 bytes "VECDATA\0"]
 * [count: uint32 little-endian]
 * [dim: uint32 little-endian]
 * [vectors: count * dim * float32 little-endian]
 *
 * @param path - Output file path
 * @param vectors - Flat Float32Array (count * dim elements)
 * @param dim - Vector dimensionality
 */
export function saveDataset(path: string, vectors: Float32Array, dim: number): void {
  const count = vectors.length / dim;
  if (vectors.length % dim !== 0) {
    throw new Error(
      `Vector array length ${vectors.length} not divisible by dimension ${dim}`
    );
  }

  // Create header (8 + 4 + 4 = 16 bytes)
  const header = Buffer.alloc(16);
  header.write(MAGIC, 0, "utf-8");
  header.writeUInt32LE(count, 8);
  header.writeUInt32LE(dim, 12);

  // Create data buffer (vectors are already Float32Array, which is little-endian)
  const data = Buffer.from(vectors.buffer);

  // Concatenate and write
  const output = Buffer.concat([header, data]);
  writeFileSync(path, output);
}

/**
 * Load dataset from binary file
 *
 * @param path - Input file path
 * @returns Dataset object
 */
export function loadDataset(path: string): Dataset {
  const buffer = readFileSync(path);

  // Validate magic header
  const magic = buffer.toString("utf-8", 0, 8);
  if (magic !== MAGIC) {
    throw new Error(
      `Invalid dataset file: magic header is "${magic}", expected "${MAGIC}"`
    );
  }

  // Read header
  const count = buffer.readUInt32LE(8);
  const dim = buffer.readUInt32LE(12);

  // Read vectors
  const dataStart = 16;
  const dataLength = count * dim * 4; // 4 bytes per float32
  if (buffer.length !== dataStart + dataLength) {
    throw new Error(
      `Invalid dataset file: expected ${dataStart + dataLength} bytes, got ${buffer.length}`
    );
  }

  const vectors = new Float32Array(
    buffer.buffer,
    buffer.byteOffset + dataStart,
    count * dim
  );

  return { vectors, dim, count };
}

/**
 * Generate random unit-normalized vectors (for cosine similarity)
 *
 * Uses Box-Muller transform to generate normally-distributed components,
 * then normalizes to unit sphere.
 *
 * @param count - Number of vectors to generate
 * @param dim - Vector dimensionality
 * @param seed - Random seed for reproducibility
 * @returns Flat Float32Array (count * dim elements)
 */
export function generateRandomVectors(
  count: number,
  dim: number,
  seed: number
): Float32Array {
  const rng = createSeededRandom(seed);
  const vectors = new Float32Array(count * dim);

  for (let i = 0; i < count; i++) {
    const offset = i * dim;
    let norm = 0;

    // Generate random components using normal distribution
    for (let j = 0; j < dim; j++) {
      vectors[offset + j] = rng.randn();
      norm += vectors[offset + j] ** 2;
    }

    // Normalize to unit sphere
    norm = Math.sqrt(norm);
    if (norm > 0) {
      for (let j = 0; j < dim; j++) {
        vectors[offset + j] /= norm;
      }
    }
  }

  return vectors;
}

/**
 * Select random indices without replacement
 *
 * @param max - Maximum index (exclusive)
 * @param count - Number of indices to select
 * @param seed - Random seed for reproducibility
 * @returns Array of unique random indices
 */
export function randomSample(max: number, count: number, seed: number): number[] {
  if (count > max) {
    throw new Error(`Cannot sample ${count} items from ${max}`);
  }

  const rng = createSeededRandom(seed);
  const selected = new Set<number>();

  while (selected.size < count) {
    const idx = Math.floor(rng.random() * max);
    selected.add(idx);
  }

  return Array.from(selected).sort((a, b) => a - b);
}
