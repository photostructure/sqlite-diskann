/**
 * Database factory for testing with multiple SQLite implementations
 *
 * Copyright 2026 PhotoStructure Inc.
 * MIT License (see LICENSE file)
 */

import type { DatabaseLike } from "../../src/types.js";

export interface DbFactory {
  name: string;
  available: boolean;
  create: (path: string) => DatabaseLike;
  cleanup?: (db: DatabaseLike) => void;
}

/**
 * Close a database if it has a close() method.
 * All three implementations (node:sqlite, better-sqlite3, @photostructure/sqlite)
 * provide close(), but it's not part of our minimal DatabaseLike interface.
 */
function closeDb(db: DatabaseLike): void {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  if ("close" in db && typeof (db as any).close === "function") {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (db as any).close();
  }
}

/**
 * Check if a module is available by trying to resolve it
 */
function isModuleAvailable(moduleName: string): boolean {
  try {
    require.resolve(moduleName);
    return true;
  } catch {
    return false;
  }
}

/**
 * Factory for @photostructure/sqlite
 */
const photostructureFactory: DbFactory = {
  name: "@photostructure/sqlite",
  available: isModuleAvailable("@photostructure/sqlite"),
  create: (path: string) => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const { DatabaseSync } = require("@photostructure/sqlite");
    // Enable extension loading
    const db = new DatabaseSync(path, { allowExtension: true });
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (db as any).enableLoadExtension?.(true);
    return db;
  },
  cleanup: closeDb,
};

/**
 * Factory for better-sqlite3
 */
const betterSqlite3Factory: DbFactory = {
  name: "better-sqlite3",
  available: isModuleAvailable("better-sqlite3"),
  create: (path: string) => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const Database = require("better-sqlite3");
    // Enable extension loading for better-sqlite3
    const db = new Database(path);
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (db as any).loadExtension = (db as any).loadExtension;
    return db;
  },
  cleanup: closeDb,
};

/**
 * Factory for node:sqlite (Node 22.5+ only, experimental).
 * Tries the actual require rather than version-checking, since availability
 * depends on both Node version and --experimental-sqlite flag.
 */
const nodeSqliteFactory: DbFactory = {
  name: "node:sqlite",
  available: (() => {
    try {
      // eslint-disable-next-line @typescript-eslint/no-require-imports
      require("node:sqlite");
      return true;
    } catch {
      return false;
    }
  })(),
  create: (path: string) => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const { DatabaseSync } = require("node:sqlite");
    // Enable extension loading for node:sqlite
    const db = new DatabaseSync(path, { allowExtension: true });
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (db as any).enableLoadExtension?.(true);
    return db;
  },
  cleanup: closeDb,
};

/**
 * All available database factories (filtered to only available ones)
 */
export const dbFactories: DbFactory[] = [
  photostructureFactory,
  betterSqlite3Factory,
  nodeSqliteFactory,
].filter((f) => f.available);
