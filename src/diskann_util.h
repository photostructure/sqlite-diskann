/*
** Shared utility functions for DiskANN
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
*/
#ifndef DISKANN_UTIL_H
#define DISKANN_UTIL_H

#include <stddef.h>

#define MAX_IDENTIFIER_LEN 64

/*
** Validate a SQL identifier (index name or database name).
** Must match [a-zA-Z_][a-zA-Z0-9_]*, max MAX_IDENTIFIER_LEN chars.
** Returns 1 if valid, 0 if invalid.
**
** SECURITY: Prevents SQL injection by validating all identifiers
** before use in dynamic SQL. TypeScript layer also validates.
*/
static inline int validate_identifier(const char *name) {
  if (!name || !name[0])
    return 0;
  char c = name[0];
  if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) {
    return 0;
  }
  size_t len = 1;
  for (const char *p = name + 1; *p; p++, len++) {
    if (len > MAX_IDENTIFIER_LEN)
      return 0;
    c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_')) {
      return 0;
    }
  }
  return 1;
}

#endif /* DISKANN_UTIL_H */
