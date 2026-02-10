/**
 * Setup verification test
 * Ensures Vitest is configured correctly
 */

import { describe, expect, it } from "vitest";

describe("Vitest Setup", () => {
  it("should run TypeScript tests", () => {
    expect(true).toBe(true);
  });

  it("should support Node.js APIs", () => {
    expect(process.version).toMatch(/^v\d+\.\d+\.\d+/);
  });

  it("should have correct Node.js version", () => {
    const nodeVersion = parseInt(process.version.slice(1).split(".")[0]);
    expect(nodeVersion).toBeGreaterThanOrEqual(20);
  });
});
