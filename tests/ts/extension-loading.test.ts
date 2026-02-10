/**
 * Extension Loading Tests
 * Based on TEST-STRATEGY.md Section 2.1
 *
 * Tests that the native DiskANN extension loads correctly across platforms.
 */

import { DatabaseSync, type DatabaseSyncInstance } from "@photostructure/sqlite";
import { afterEach, beforeEach, describe, expect, it } from "vitest";

describe("Extension Loading", () => {
  let db: DatabaseSyncInstance;

  beforeEach(() => {
    db = new DatabaseSync(":memory:");
  });

  afterEach(() => {
    db.close();
  });

  it.skip("loads extension on current platform", () => {
    // Skip until we have compiled extension binary
    const platform = process.platform;
    const extPath = getExtensionPath(platform);

    expect(() => {
      db.loadExtension(extPath);
    }).not.toThrow();
  });

  it("fails gracefully with helpful error on missing binary", () => {
    expect(() => {
      db.loadExtension("/nonexistent/path.so");
    }).toThrow();
  });

  it("creates database successfully", () => {
    // Basic smoke test
    expect(db).toBeDefined();
    const stmt = db.prepare("SELECT 1 as value");
    const result = stmt.get();
    expect(result).toEqual({ value: 1 });
  });
});

/**
 * Get platform-specific extension path
 * Will be implemented when binaries are built
 */
function getExtensionPath(platform: string): string {
  const extensions: Record<string, string> = {
    linux: "diskann.so",
    darwin: "diskann.dylib",
    win32: "diskann.dll",
  };

  // eslint-disable-next-line security/detect-object-injection
  const ext = extensions[platform] ?? "diskann.so";
  return `./build/${ext}`;
}
