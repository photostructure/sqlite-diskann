# SQLite Implementation Compatibility

## Overview

sqlite-diskann uses **duck typing** to support multiple SQLite library implementations. Any library providing the minimal required interface will work automatically, without needing to be explicitly listed as a dependency.

## Required Interface

Your SQLite library must provide these methods:

```typescript
interface DatabaseLike {
  loadExtension(path: string, entryPoint?: string): void;
  exec(sql: string): void;
  prepare(sql: string): StatementLike;
}

interface StatementLike {
  run(...params: any[]): { changes: number; lastInsertRowid: number | bigint };
  all(...params: any[]): any[];
}
```

This is the **complete** interface required - just 3 database methods and 2 statement methods.

## Tested Implementations

### node:sqlite (Node 22.5+, experimental)

- **Module**: Built-in to Node.js (since 22.5.0)
- **Class**: `DatabaseSync`
- **Compatibility**: ✅ 100% compatible
- **Performance**: Fastest (no C++ addon loading overhead)
- **Installation**: None (built-in)
- **Status**: ⚠️ **Experimental** (stability level 1.1 - Active development)

```typescript
import { DatabaseSync } from "node:sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);
```

**Requirements**:

- Node.js >= 22.5.0
- `--experimental-sqlite` flag required
- Still experimental as of this writing (not recommended for production)

**Run with:**

```bash
node --experimental-sqlite your-script.js
```

### better-sqlite3

- **Module**: `better-sqlite3` npm package
- **Class**: `Database`
- **Compatibility**: ✅ 100% compatible
- **Performance**: Excellent
- **Installation**: `npm install better-sqlite3`

```typescript
import Database from "better-sqlite3";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new Database(":memory:");
loadDiskAnnExtension(db);
```

### @photostructure/sqlite

- **Module**: `@photostructure/sqlite` npm package
- **Class**: `DatabaseSync`
- **Compatibility**: ✅ 100% compatible
- **Performance**: Identical to node:sqlite
- **Installation**: `npm install @photostructure/sqlite`

```typescript
import { DatabaseSync } from "@photostructure/sqlite";
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new DatabaseSync(":memory:", { allowExtension: true });
loadDiskAnnExtension(db);
```

**Note**: This is an independent implementation, written from scratch, that is strictly API-compatible with the latest version of Node.js's `node:sqlite` module. It supports older Node.js versions that lack the built-in `node:sqlite` module.

## Known Differences Between Implementations

All three implementations are compatible, but there are minor differences to be aware of:

### Return Type: `lastInsertRowid`

All three implementations return `{ changes: number, lastInsertRowid: number | bigint }` from `stmt.run()`. The `lastInsertRowid` field can be either `number` or `bigint` depending on the row ID value.

**Recommended handling:**

```typescript
const { lastInsertRowid } = stmt.run(params);
const id =
  typeof lastInsertRowid === "bigint" ? Number(lastInsertRowid) : lastInsertRowid;
```

### Error Messages

Each implementation throws different error types and messages. Your error handling should check message content rather than error type:

```typescript
try {
  loadDiskAnnExtension(db);
} catch (error) {
  console.error("Failed to load extension:", error.message);
}
```

### Extension Loading

- **node:sqlite**: Requires `--experimental-sqlite` flag (all versions, still experimental)
- **better-sqlite3**: No flags needed, stable
- **@photostructure/sqlite**: No flags needed, stable

## Using Your Own SQLite Wrapper

If you have a custom SQLite wrapper, it will work with sqlite-diskann if it provides the required interface:

```typescript
class MyCustomDatabase {
  loadExtension(path: string, entryPoint?: string): void {
    // Your implementation
  }

  exec(sql: string): void {
    // Your implementation
  }

  prepare(sql: string): MyCustomStatement {
    // Your implementation
    return new MyCustomStatement(sql);
  }
}

class MyCustomStatement {
  run(...params: any[]): { changes: number; lastInsertRowid: number | bigint } {
    // Your implementation
    return { changes: 1, lastInsertRowid: 123 };
  }

  all(...params: any[]): any[] {
    // Your implementation
    return [];
  }
}
```

Then use it with sqlite-diskann:

```typescript
import { loadDiskAnnExtension } from "@photostructure/sqlite-diskann";

const db = new MyCustomDatabase();
loadDiskAnnExtension(db); // Works!
```

## Type Safety

TypeScript will accept any object with the correct shape:

```typescript
// This type-checks but will fail at runtime
const fakeDb = {
  exec: () => {},
  prepare: () => ({ run: () => ({}), all: () => [] }),
  loadExtension: () => {
    throw new Error("Not implemented");
  },
};

loadDiskAnnExtension(fakeDb); // Compiles, throws at runtime
```

This is intentional - duck typing trades compile-time safety for runtime flexibility. The benefit is that **any** SQLite library works without modification.

## Migration Guide

### Switching Between Implementations

All three implementations have identical APIs for the methods we use. To switch:

#### From @photostructure/sqlite to node:sqlite (Node 22+)

```bash
npm uninstall @photostructure/sqlite
```

```diff
-import { DatabaseSync } from "@photostructure/sqlite";
+import { DatabaseSync } from "node:sqlite";

 const db = new DatabaseSync(":memory:", { allowExtension: true });
 // Rest of code unchanged
```

#### From @photostructure/sqlite to better-sqlite3

```bash
npm uninstall @photostructure/sqlite
npm install better-sqlite3
```

```diff
-import { DatabaseSync } from "@photostructure/sqlite";
+import Database from "better-sqlite3";

-const db = new DatabaseSync(":memory:", { allowExtension: true });
+const db = new Database(":memory:");
 // Rest of code unchanged
```

## Advantages of Duck Typing

### No Peer Dependency Warnings

Without peer dependencies, users never see warnings like:

```
npm WARN sqlite-diskann@1.0.0 requires a peer of better-sqlite3@>=11.0.0
```

### Zero Dependencies

Users with Node 22+ don't need to install any additional packages. The package has **zero runtime dependencies**.

### Future-Proof

If a new SQLite library is released tomorrow with the same interface, it will work with sqlite-diskann without any changes.

### Flexibility

Users can:

- Mock the database for testing
- Use wrapped/proxied database instances
- Switch implementations without changing application code
- Use internal/enterprise SQLite libraries

## Testing

The test suite runs against all available implementations automatically:

```bash
npm run test:ts
```

Each available implementation gets its own test suite. If an implementation is not installed or not available on the current Node version (e.g., `node:sqlite` on Node 20), those tests are automatically skipped.

## Summary

- **Interface**: Only 3 database methods + 2 statement methods required
- **No dependencies**: Users choose their own SQLite library
- **100% compatible**: node:sqlite, better-sqlite3, @photostructure/sqlite all work
- **Future-proof**: Any library matching the interface will work
- **Type-safe**: TypeScript ensures compile-time compatibility
