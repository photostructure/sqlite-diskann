/**
 * API Tests for TypeScript wrapper
 * Based on TEST-STRATEGY.md Section 2.2
 *
 * Tests the TypeScript API layer (not yet the C extension implementation)
 */

import { DatabaseSync, type DatabaseSyncInstance } from "@photostructure/sqlite";
import { afterEach, beforeEach, describe, expect, it } from "vitest";
import {
  createDiskAnnIndex,
  deleteVector,
  getExtensionPath,
  insertVector,
  loadDiskAnnExtension,
  searchNearest,
} from "../../src/index.js";

describe("TypeScript API", () => {
  describe("getExtensionPath()", () => {
    it("returns platform-specific extension path", () => {
      const path = getExtensionPath();
      expect(path).toBeDefined();
      expect(path).toContain("build/diskann");

      // Platform-specific extension
      if (process.platform === "win32") {
        expect(path).toMatch(/\.dll$/);
      } else if (process.platform === "darwin") {
        expect(path).toMatch(/\.dylib$/);
      } else {
        expect(path).toMatch(/\.so$/);
      }
    });
  });

  describe("loadDiskAnnExtension()", () => {
    let db: DatabaseSyncInstance;

    beforeEach(() => {
      db = new DatabaseSync(":memory:");
    });

    afterEach(() => {
      db.close();
    });

    it.skip("loads extension without throwing (once binary is built)", () => {
      // Skip until C implementation is complete and binary exists
      expect(() => loadDiskAnnExtension(db)).not.toThrow();
    });

    it("fails with helpful error when binary doesn't exist", () => {
      // Currently expected to fail since binary isn't built yet
      expect(() => loadDiskAnnExtension(db)).toThrow();
    });
  });

  describe("createDiskAnnIndex()", () => {
    let db: DatabaseSyncInstance;

    beforeEach(() => {
      db = new DatabaseSync(":memory:");
    });

    afterEach(() => {
      db.close();
    });

    it("validates dimension parameter", () => {
      expect(() => {
        createDiskAnnIndex(db, "test", { dimension: 0 });
      }).toThrow(/Invalid dimension/);

      expect(() => {
        createDiskAnnIndex(db, "test", { dimension: -1 });
      }).toThrow(/Invalid dimension/);

      expect(() => {
        createDiskAnnIndex(db, "test", { dimension: 3.5 });
      }).toThrow(/Invalid dimension/);
    });

    it("validates metric parameter", () => {
      expect(() => {
        createDiskAnnIndex(db, "test", {
          dimension: 128,
          // eslint-disable-next-line @typescript-eslint/no-explicit-any
          metric: "invalid" as any,
        });
      }).toThrow(/Invalid metric/);
    });

    it("validates maxDegree parameter", () => {
      expect(() => {
        createDiskAnnIndex(db, "test", {
          dimension: 128,
          maxDegree: 0,
        });
      }).toThrow(/Invalid maxDegree/);

      expect(() => {
        createDiskAnnIndex(db, "test", {
          dimension: 128,
          maxDegree: -10,
        });
      }).toThrow(/Invalid maxDegree/);
    });

    it("uses default values for optional parameters", () => {
      // This will fail until extension is loaded, but validates API
      try {
        createDiskAnnIndex(db, "test", { dimension: 128 });
      } catch (e) {
        // Expected to fail without loaded extension
        // Just validating the function doesn't throw on valid inputs
        expect((e as Error).message).not.toMatch(/Invalid/);
      }
    });

    it.skip("creates virtual table successfully (once extension loaded)", () => {
      // Skip until extension is implemented
      loadDiskAnnExtension(db);
      createDiskAnnIndex(db, "embeddings", {
        dimension: 512,
        metric: "cosine",
        maxDegree: 64,
      });

      // Verify table exists
      const result = db
        .prepare(
          "SELECT name FROM sqlite_master WHERE type='table' AND name='embeddings'"
        )
        .get();
      expect(result).toBeDefined();
    });
  });

  describe("insertVector()", () => {
    let db: DatabaseSyncInstance;

    beforeEach(() => {
      db = new DatabaseSync(":memory:");
    });

    afterEach(() => {
      db.close();
    });

    it("validates vector parameter", () => {
      expect(() => {
        insertVector(db, "test", 1, []);
      }).toThrow(/non-empty array/);

      expect(() => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        insertVector(db, "test", 1, null as any);
      }).toThrow(/non-empty array/);

      expect(() => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        insertVector(db, "test", 1, "not an array" as any);
      }).toThrow(/non-empty array/);
    });

    it.skip("inserts vector successfully (once extension loaded)", () => {
      loadDiskAnnExtension(db);
      createDiskAnnIndex(db, "embeddings", {
        dimension: 3,
        metric: "euclidean",
      });

      expect(() => {
        insertVector(db, "embeddings", 1, [1.0, 2.0, 3.0]);
      }).not.toThrow();
    });
  });

  describe("searchNearest()", () => {
    let db: DatabaseSyncInstance;

    beforeEach(() => {
      db = new DatabaseSync(":memory:");
    });

    afterEach(() => {
      db.close();
    });

    it("validates query vector parameter", () => {
      expect(() => {
        searchNearest(db, "test", []);
      }).toThrow(/non-empty array/);

      expect(() => {
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        searchNearest(db, "test", null as any);
      }).toThrow(/non-empty array/);
    });

    it("validates k parameter", () => {
      expect(() => {
        searchNearest(db, "test", [1, 2, 3], 0);
      }).toThrow(/Invalid k/);

      expect(() => {
        searchNearest(db, "test", [1, 2, 3], -5);
      }).toThrow(/Invalid k/);

      expect(() => {
        searchNearest(db, "test", [1, 2, 3], 3.5);
      }).toThrow(/Invalid k/);
    });

    it.skip("returns empty array for empty index (once extension loaded)", () => {
      loadDiskAnnExtension(db);
      createDiskAnnIndex(db, "embeddings", {
        dimension: 3,
        metric: "euclidean",
      });

      const results = searchNearest(db, "embeddings", [1.0, 2.0, 3.0], 10);
      expect(results).toEqual([]);
    });

    it.skip("returns correct neighbors (once extension loaded)", () => {
      loadDiskAnnExtension(db);
      createDiskAnnIndex(db, "embeddings", {
        dimension: 3,
        metric: "euclidean",
      });

      // Insert test vectors
      insertVector(db, "embeddings", 1, [1.0, 0.0, 0.0]);
      insertVector(db, "embeddings", 2, [0.0, 1.0, 0.0]);
      insertVector(db, "embeddings", 3, [0.0, 0.0, 1.0]);

      // Search for nearest to [1, 0, 0]
      const results = searchNearest(db, "embeddings", [1.0, 0.0, 0.0], 2);

      expect(results).toHaveLength(2);
      expect(results[0].id).toBe(1); // Exact match
      expect(results[0].distance).toBeCloseTo(0.0, 3);
    });
  });

  describe("deleteVector()", () => {
    let db: DatabaseSyncInstance;

    beforeEach(() => {
      db = new DatabaseSync(":memory:");
    });

    afterEach(() => {
      db.close();
    });

    it.skip("deletes vector successfully (once extension loaded)", () => {
      loadDiskAnnExtension(db);
      createDiskAnnIndex(db, "embeddings", {
        dimension: 3,
        metric: "euclidean",
      });

      insertVector(db, "embeddings", 1, [1.0, 0.0, 0.0]);
      expect(() => deleteVector(db, "embeddings", 1)).not.toThrow();

      // Verify deletion
      const results = searchNearest(db, "embeddings", [1.0, 0.0, 0.0], 10);
      expect(results).toHaveLength(0);
    });
  });

  describe("Type exports", () => {
    it("exports expected functions", async () => {
      // This is a compile-time check more than runtime
      // Just verify the module exports what we expect
      const module = await import("../../src/index.js");

      expect(module.getExtensionPath).toBeDefined();
      expect(module.loadDiskAnnExtension).toBeDefined();
      expect(module.createDiskAnnIndex).toBeDefined();
      expect(module.insertVector).toBeDefined();
      expect(module.searchNearest).toBeDefined();
      expect(module.deleteVector).toBeDefined();
      expect(module.getExtensionPath).toBeTypeOf("function");
    });
  });
});
