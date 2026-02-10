/**
 * Virtual Table Creation Tests
 *
 * Tests for the bug fix in 20260210-vtab-create-index-failure.md
 * Verifies that CREATE VIRTUAL TABLE works when called through the extension,
 * which previously failed with "cannot open savepoint - SQL statements in progress"
 */

import { afterEach, beforeEach, describe, expect, it } from "vitest";
import type { DatabaseLike } from "../../src/index.js";
import { dbFactories } from "./db-factory.js";

function getExtensionPath(): string {
  // Extension is built to build/diskann.so
  return "./build/diskann";
}

// Run tests against all available implementations
for (const factory of dbFactories) {
  describe(`Virtual Table Creation with ${factory.name}`, () => {
    let db: DatabaseLike;

    beforeEach(() => {
      db = factory.create(":memory:");
    });

    afterEach(() => {
      factory.cleanup?.(db);
    });

    it("loads extension successfully", () => {
      const extPath = getExtensionPath();
      expect(() => {
        db.loadExtension(extPath);
      }).not.toThrow();
    });

    it("creates virtual table successfully", () => {
      const extPath = getExtensionPath();
      db.loadExtension(extPath);

      // This is the exact operation that failed before the fix
      expect(() => {
        db.exec(
          "CREATE VIRTUAL TABLE test_vectors USING diskann(dimension=10, metric=cosine)"
        );
      }).not.toThrow();
    });

    it("creates virtual table with different configurations", () => {
      const extPath = getExtensionPath();
      db.loadExtension(extPath);

      // Test with euclidean metric
      expect(() => {
        db.exec(
          "CREATE VIRTUAL TABLE euclidean_vecs USING diskann(dimension=128, metric=euclidean)"
        );
      }).not.toThrow();

      // Test with dot product metric
      expect(() => {
        db.exec(
          "CREATE VIRTUAL TABLE dot_vecs USING diskann(dimension=64, metric=dot)"
        );
      }).not.toThrow();
    });

    it("fails with helpful error when dimension missing", () => {
      const extPath = getExtensionPath();
      db.loadExtension(extPath);

      expect(() => {
        db.exec("CREATE VIRTUAL TABLE bad_table USING diskann(metric=cosine)");
      }).toThrow(/dimension parameter required/i);
    });

    it("creates multiple virtual tables in same database", () => {
      const extPath = getExtensionPath();
      db.loadExtension(extPath);

      db.exec("CREATE VIRTUAL TABLE table1 USING diskann(dimension=10, metric=cosine)");
      db.exec(
        "CREATE VIRTUAL TABLE table2 USING diskann(dimension=20, metric=euclidean)"
      );

      // Both should exist (query shadow tables)
      const stmt = db.prepare(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE '%_shadow'"
      );
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const tables = (stmt as any).all();
      expect(tables).toHaveLength(2);
    });

    it("creates virtual table in file database", () => {
      // Test with file database (not just :memory:)
      const fileDb = factory.create("/tmp/test-vtab-creation.db");
      const extPath = getExtensionPath();

      try {
        fileDb.loadExtension(extPath);
        // Drop table if it exists from previous test run
        try {
          fileDb.exec("DROP TABLE IF EXISTS file_vectors");
        } catch {
          // Ignore errors
        }
        expect(() => {
          fileDb.exec(
            "CREATE VIRTUAL TABLE file_vectors USING diskann(dimension=256, metric=cosine)"
          );
        }).not.toThrow();
      } finally {
        factory.cleanup?.(fileDb);
      }
    });

    it("shadow tables are created correctly", () => {
      const extPath = getExtensionPath();
      db.loadExtension(extPath);

      db.exec("CREATE VIRTUAL TABLE vecs USING diskann(dimension=10, metric=cosine)");

      // Check shadow table exists
      const shadowCheck = db.prepare(
        "SELECT name FROM sqlite_master WHERE name='vecs_shadow'"
      );
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const shadow = (shadowCheck as any).get();
      expect(shadow).toBeDefined();

      // Check metadata table exists
      const metadataCheck = db.prepare(
        "SELECT name FROM sqlite_master WHERE name='vecs_metadata'"
      );
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const metadata = (metadataCheck as any).get();
      expect(metadata).toBeDefined();

      // Verify metadata contains configuration
      const configCheck = db.prepare(
        "SELECT key, value FROM vecs_metadata WHERE key='dimensions'"
      );
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const dimRow = (configCheck as any).get();
      expect(dimRow).toEqual({ key: "dimensions", value: 10 });
    });
  });
}
