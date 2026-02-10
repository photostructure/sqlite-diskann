import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    globals: true,
    environment: "node",
    include: ["tests/ts/**/*.test.ts"],
    exclude: ["tests/c/**", "node_modules", "dist", "build"],
    coverage: {
      provider: "v8",
      reporter: ["text", "json", "html", "lcov"],
      include: ["src/**/*.ts"],
      exclude: ["tests/**", "**/*.test.ts", "**/*.config.ts", "dist/**", "build/**"],
      all: true,
      lines: 90,
      functions: 90,
      branches: 90,
      statements: 90,
    },
    testTimeout: 30000, // 30s for integration tests with large datasets
    hookTimeout: 30000,
  },
});
