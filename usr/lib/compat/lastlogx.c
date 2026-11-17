/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * getlastlogx and updlastlogx, NetBSD's interface to each user's last
 * login, for login's "Last login:" line: a db(3) hash file (by default
 * /var/adm/lastlogx, beside utmpx and wtmpx) whose keys are user IDs
 * and whose data are struct lastlogx (see compat.h). Readers take a
 * shared flock lock on it, the writer an exclusive one.
 */

#include <db.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>

#include "compat.h"

/* The record for uid in the file at path, in *ll; ll, or NULL. */
struct lastlogx *
getlastlogx(const char *path, uid_t uid, struct lastlogx *ll)
{
        DBT key, data;
        DB *db;
        int r;

        if ((db = dbopen(path, O_RDONLY, 0, DB_HASH, NULL)) == NULL)
                return NULL;
        (void)flock(db->fd(db), LOCK_SH);
        key.data = &uid;
        key.size = sizeof(uid);
        r = db->get(db, &key, &data, 0);
        if (r == 0 && data.size == sizeof(*ll))
                memcpy(ll, data.data, sizeof(*ll));
        else if (r == 0)
                r = 1;          /* not one of ours */
        (void)db->close(db);
        return r == 0 ? ll : NULL;
}

/* Make *ll uid's record in the file at path, making it if need be. */
int
updlastlogx(const char *path, uid_t uid, struct lastlogx *ll)
{
        DBT key, data;
        DB *db;
        int r, e;

        if ((db = dbopen(path, O_RDWR | O_CREAT, 0644, DB_HASH, NULL)) ==
            NULL)
                return -1;
        (void)flock(db->fd(db), LOCK_EX);
        key.data = &uid;
        key.size = sizeof(uid);
        data.data = ll;
        data.size = sizeof(*ll);
        r = db->put(db, &key, &data, 0);
        e = errno;
        if (db->close(db) != 0 && r == 0) {
                r = -1;
                e = errno;
        }
        errno = e;
        return r == 0 ? 0 : -1;
}
