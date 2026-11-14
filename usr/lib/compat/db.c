/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * dbopen(NULL, ..., DB_HASH, NULL): an in-memory table of copied keys
 * and data, kept as a list; enough for init's table of sessions. See
 * <db.h>.
 */

#include <db.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct entry {
        struct entry    *next;
        DBT             key;
        DBT             data;
};

struct table {
        struct entry    *head;
};

static struct entry **
lookup(const DB *db, const DBT *key)
{
        struct table *t = db->internal;
        struct entry **link;

        for (link = &t->head; *link != NULL; link = &(*link)->next) {
                const DBT *k = &(*link)->key;

                if (k->size == key->size &&
                    memcmp(k->data, key->data, key->size) == 0)
                        break;
        }
        return link;
}

static void *
copy(const DBT *d)
{
        void *p = malloc(d->size != 0 ? d->size : 1);

        if (p != NULL && d->size != 0)
                memcpy(p, d->data, d->size);
        return p;
}

static int
mem_get(const DB *db, const DBT *key, DBT *data, unsigned int flags)
{
        struct entry *e;

        if (flags != 0) {
                errno = EINVAL;
                return RET_ERROR;
        }
        if ((e = *lookup(db, key)) == NULL)
                return RET_SPECIAL;
        *data = e->data;
        return RET_SUCCESS;
}

static int
mem_put(const DB *db, DBT *key, const DBT *data, unsigned int flags)
{
        struct entry **link = lookup(db, key), *e = *link;
        void *d;

        if ((flags & ~R_NOOVERWRITE) != 0) {
                errno = EINVAL;
                return RET_ERROR;
        }
        if (e != NULL && (flags & R_NOOVERWRITE) != 0)
                return RET_SPECIAL;
        if ((d = copy(data)) == NULL)
                return RET_ERROR;
        if (e == NULL) {
                if ((e = calloc(1, sizeof(*e))) == NULL ||
                    (e->key.data = copy(key)) == NULL) {
                        free(e);
                        free(d);
                        return RET_ERROR;
                }
                e->key.size = key->size;
                *link = e;
        } else {
                free(e->data.data);
        }
        e->data.data = d;
        e->data.size = data->size;
        return RET_SUCCESS;
}

static int
mem_del(const DB *db, const DBT *key, unsigned int flags)
{
        struct entry **link = lookup(db, key), *e = *link;

        if (flags != 0) {
                errno = EINVAL;
                return RET_ERROR;
        }
        if (e == NULL)
                return RET_SPECIAL;
        *link = e->next;
        free(e->key.data);
        free(e->data.data);
        free(e);
        return RET_SUCCESS;
}

static int
mem_close(DB *db)
{
        struct table *t = db->internal;

        while (t->head != NULL) {
                struct entry *e = t->head;

                t->head = e->next;
                free(e->key.data);
                free(e->data.data);
                free(e);
        }
        free(t);
        free(db);
        return RET_SUCCESS;
}

static int
mem_seq(const DB *db, DBT *key, DBT *data, unsigned int flags)
{
        (void)db;
        (void)key;
        (void)data;
        (void)flags;
        errno = ENOTSUP;
        return RET_ERROR;
}

static int
mem_sync(const DB *db, unsigned int flags)
{
        (void)db;
        (void)flags;
        return RET_SUCCESS;
}

static int
mem_fd(const DB *db)
{
        (void)db;
        errno = ENOENT;
        return -1;
}

DB *
dbopen(const char *file, int flags, int mode, DBTYPE type,
    const void *info)
{
        DB *db;

        (void)flags;
        (void)mode;
        if (file != NULL || type != DB_HASH || info != NULL) {
                errno = EINVAL;
                return NULL;
        }
        if ((db = calloc(1, sizeof(*db))) == NULL)
                return NULL;
        if ((db->internal = calloc(1, sizeof(struct table))) == NULL) {
                free(db);
                return NULL;
        }
        db->type = type;
        db->close = mem_close;
        db->del = mem_del;
        db->get = mem_get;
        db->put = mem_put;
        db->seq = mem_seq;
        db->sync = mem_sync;
        db->fd = mem_fd;
        return db;
}
