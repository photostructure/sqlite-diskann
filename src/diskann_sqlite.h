/*
** SQLite API access for DiskANN extension
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** MULTI-FILE EXTENSION PROBLEM:
** ==============================
** DiskANN is a multi-file extension (6 .c files all calling SQLite functions).
** When building as extension, ALL SQLite calls must be routed through the
** sqlite3_api function pointer table. This header solves the problem:
**
** Extension builds (-DDISKANN_EXTENSION):
**   - Includes <sqlite3ext.h> which provides routing macros
**   - One file (diskann_vtab.c) defines DISKANN_VTAB_MAIN and owns the
**     sqlite3_api definition via SQLITE_EXTENSION_INIT1
**   - Other files get extern declaration to access the same pointer table
**
** Test builds (no flag):
**   - Includes <sqlite3.h> directly for native linking with vendored SQLite
**   - No extension macros needed
**
** This is NOT optional - removing it will cause "undefined symbol" errors.
** See _todo/20260210-extension-loading-fix.md for full explanation.
*/

#ifndef DISKANN_SQLITE_H
#define DISKANN_SQLITE_H

#ifdef DISKANN_EXTENSION
/* Building as SQLite extension */
#include <sqlite3ext.h>

/* For multi-file extensions, only the main file (diskann_vtab.c) has
** SQLITE_EXTENSION_INIT1 which creates the definition. Other files need
** to declare it as extern to access the same instance. */
#ifndef DISKANN_VTAB_MAIN
extern const sqlite3_api_routines *sqlite3_api;
#endif

#else
/* Building standalone (tests) */
#include <sqlite3.h>
#endif

#endif /* DISKANN_SQLITE_H */
