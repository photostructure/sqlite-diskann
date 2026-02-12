/**
 * Seeded random number generator for reproducible benchmarks
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * Seeded random number generator
 */
export interface SeededRandom {
  /** Generate random number in [0, 1) */
  random(): number;
  /** Generate random number from standard normal distribution (mean=0, stddev=1) */
  randn(): number;
}

/**
 * Create a seeded random number generator
 *
 * Uses splitmix64 algorithm (from photostructure/sqlite-seeded-random)
 * which has excellent statistical properties and full 2^64 period.
 *
 * @param seed - Seed value
 * @returns Seeded random number generator
 */
export function createSeededRandom(seed: number): SeededRandom {
  // Initialize splitmix64 state
  // JavaScript doesn't have true 64-bit integers, so we use two 32-bit parts
  let state = seed >>> 0; // Ensure unsigned 32-bit

  // For Box-Muller transform
  let spare: number | null = null;

  return {
    random(): number {
      // Splitmix64 algorithm (adapted for 32-bit JavaScript)
      // Golden ratio constant (lower 32 bits of 0x9E3779B97F4A7C15)
      state = (state + 0x7f4a7c15) >>> 0;

      let z = state;
      z = ((z ^ (z >>> 16)) * 0x85ebca6b) >>> 0;
      z = ((z ^ (z >>> 13)) * 0xc2b2ae35) >>> 0;
      z = (z ^ (z >>> 16)) >>> 0;

      // Convert to [0, 1)
      return z / 0x100000000;
    },

    randn(): number {
      // Box-Muller transform to generate standard normal distribution
      if (spare !== null) {
        const result = spare;
        spare = null;
        return result;
      }

      let u: number, v: number, s: number;
      do {
        u = this.random() * 2 - 1;
        v = this.random() * 2 - 1;
        s = u * u + v * v;
      } while (s >= 1 || s === 0);

      const mul = Math.sqrt((-2 * Math.log(s)) / s);
      spare = v * mul;
      return u * mul;
    },
  };
}
