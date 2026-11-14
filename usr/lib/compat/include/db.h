/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The part of BSD's db(3) that imported programs use: an in-memory
 * table opened with dbopen(NULL, ..., DB_HASH, NULL), with get, put,
 * del and close. There are no files, cursors (seq), or other types.
 */

#ifndef _COMPAT_DB_H_
#define _COMPAT_DB_H_

#include <stddef.h>
#include <stdint.h>

#define RET_ERROR       -1
#define RET_SUCCESS     0
#define RET_SPECIAL     1       /* get: not found; put R_NOOVERWRITE: there */

#define R_NOOVERWRITE   8       /* put: fail if the key is there */

typedef enum { DB_BTREE, DB_HASH, DB_RECNO } DBTYPE;

typedef struct {
        void            *data;
        size_t          size;
} DBT;

typedef struct __db {
        DBTYPE          type;
        int             (*close)(struct __db *);
        int             (*del)(const struct __db *, const DBT *, unsigned int);
        int             (*get)(const struct __db *, const DBT *, DBT *,
                            unsigned int);
        int             (*put)(const struct __db *, DBT *, const DBT *,
                            unsigned int);
        int             (*seq)(const struct __db *, DBT *, DBT *,
                            unsigned int);
        int             (*sync)(const struct __db *, unsigned int);
        void            *internal;
        int             (*fd)(const struct __db *);
} DB;

#ifdef __cplusplus
extern "C" {
#endif

DB *dbopen(const char *, int, int, DBTYPE, const void *);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_DB_H_ */
