import pluginJs from "@eslint/js";
import regexp_plugin from "eslint-plugin-regexp";
import security from "eslint-plugin-security";
import globals from "globals";
import ts_eslint from "typescript-eslint";

/** @type {import('eslint').Linter.Config[]} */
export default [
  {
    files: ["tests/**/*.ts"],
    languageOptions: {
      globals: {
        ...globals.node,
      },
    },
  },
  {
    ignores: [
      "build",
      "coverage",
      "dist",
      "vendor",
      "node_modules",
      "*.cjs",
      "**/*.cjs",
    ],
  },
  pluginJs.configs.recommended,
  ...ts_eslint.configs.recommended,
  ...ts_eslint.configs.strict,
  regexp_plugin.configs["flat/recommended"],
  {
    plugins: {
      security,
    },
    rules: {
      "@typescript-eslint/no-shadow": "error",
      "security/detect-object-injection": "warn",
      "security/detect-non-literal-require": "warn",
      "security/detect-eval-with-expression": "error",
      "security/detect-non-literal-regexp": "warn",
      "security/detect-unsafe-regex": "error",
      "security/detect-buffer-noassert": "error",
      "security/detect-child-process": "warn",
      "security/detect-disable-mustache-escape": "error",
      "security/detect-no-csrf-before-method-override": "error",
      "security/detect-possible-timing-attacks": "warn",
      "security/detect-pseudoRandomBytes": "error",
    },
  },
  {
    files: ["**/*.test.ts"],
    rules: {
      "@typescript-eslint/no-non-null-assertion": "off",
      "security/detect-object-injection": "off", // False positives on array indexing in tests
    },
  },
];
