# TypeScript Tests

TypeScript/Node.js integration tests for the sqlite-diskann npm package.

## Test Framework

**Vitest** - Modern, fast TypeScript testing framework with native ESM support.

## Test Categories

Based on [TEST-STRATEGY.md](../TEST-STRATEGY.md) Section 2 (Layer 2: TypeScript/Node.js Tests):

### Current Tests

- **setup.test.ts** - Vitest configuration verification
- **extension-loading.test.ts** - Native extension loading tests (partial)

### Planned Tests

- **api-wrapper.test.ts** - DiskANN API wrapper
- **types.test.ts** - TypeScript type definitions
- **platform-binaries.test.ts** - Cross-platform binary selection
- **integration.test.ts** - End-to-end scenarios (10K vectors, persistence)
- **package.test.ts** - npm package structure

## Running Tests

```bash
# Install dependencies first
npm install

# Run all TypeScript tests
npm run test:ts

# Watch mode (re-run on file changes)
npm run test:watch

# With coverage report
npm run test:coverage

# Run all tests (C + TypeScript)
npm test
```

## Test Environment

- **Node.js:** >=20.0.0
- **Runtime:** @photostructure/sqlite (drop-in replacement for node:sqlite)
- **Coverage:** Vitest coverage-v8 (target: >90% line coverage)

## Writing Tests

Follow patterns from TEST-STRATEGY.md:

```typescript
import { describe, expect, it } from "vitest";
import { DatabaseSync } from "@photostructure/sqlite";

describe("Feature Name", () => {
  it("should do something", () => {
    const db = new DatabaseSync(":memory:");
    // Test implementation
    expect(result).toBeDefined();
  });
});
```

## Coverage Goals

- **Target:** >90% line coverage (TypeScript is easier than C)
- **Critical paths:** 100% coverage
  - Extension loading
  - API wrapper
  - Error handling

## Notes

- Tests currently use `.skip()` for extension loading until binaries are compiled
- See TEST-STRATEGY.md for complete test plan
- Reference projects: `../fs-metadata`, `../node-sqlite`
