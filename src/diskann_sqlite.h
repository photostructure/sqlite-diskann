/*
** SQLite API access for DiskANN extension
**
** Copyright 2026 PhotoStructure Inc.
** MIT License
**
** When building as extension (DISKANN_EXTENSION defined):
**   - Include sqlite3ext.h and use extension API routing
**   - All SQLite calls go through function pointers set by
*SQLITE_EXTENSION_INIT2
**   - SQLITE_EXTENSION_INIT1 appears in EXACTLY ONE file (diskann_vtab.c)
**   - Other files use the extern declaration below
**
** When building standalone (tests):
**   - Include sqlite3.h directly
**   - All SQLite calls are direct function calls
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
