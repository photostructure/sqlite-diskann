/**
 * Extension Loading Tests
 * Based on TEST-STRATEGY.md Section 2.1
 *
 * Tests that the native DiskANN extension loads correctly across platforms.
 */

import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { DatabaseLike } from "../../src/index.js";
import { dbFactories } from "./db-factory.js";

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

  const ext = extensions[platform] ?? "diskann.so";
  return `./build/${ext}`;
}

// Run tests against all available implementations
for (const factory of dbFactories) {
  describe(`Extension Loading with ${factory.name}`, () => {
    let db: DatabaseLike;

    beforeEach(() => {
      db = factory.create(":memory:");
    });

    afterEach(() => {
      factory.cleanup?.(db);
    });

    it("loads extension on current platform", () => {
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
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const result = (stmt as any).get(); // .get() not in minimal StatementLike interface
      expect(result).toEqual({ value: 1 });
    });
  });
}
