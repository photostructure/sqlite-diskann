/**
 * High-resolution timing utilities
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

/**
 * High-resolution timer for benchmarking
 */
export class Timer {
  private startTime: number = 0;

  /**
   * Start the timer
   */
  start(): void {
    this.startTime = performance.now();
  }

  /**
   * Stop the timer and return elapsed time
   *
   * @returns Elapsed time in milliseconds
   */
  stop(): number {
    return performance.now() - this.startTime;
  }

  /**
   * Get elapsed time without stopping
   *
   * @returns Elapsed time in milliseconds
   */
  elapsed(): number {
    return performance.now() - this.startTime;
  }
}

/**
 * Time a synchronous function
 *
 * @param fn - Function to time
 * @returns Tuple of [result, elapsed time in ms]
 */
export function time<T>(fn: () => T): [T, number] {
  const timer = new Timer();
  timer.start();
  const result = fn();
  const elapsed = timer.stop();
  return [result, elapsed];
}

/**
 * Time an asynchronous function
 *
 * @param fn - Async function to time
 * @returns Tuple of [result, elapsed time in ms]
 */
export async function timeAsync<T>(fn: () => Promise<T>): Promise<[T, number]> {
  const timer = new Timer();
  timer.start();
  const result = await fn();
  const elapsed = timer.stop();
  return [result, elapsed];
}
