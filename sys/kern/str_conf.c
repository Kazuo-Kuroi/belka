/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * The STREAMS modules that may be pushed, and the STREAMS devices: the
 * pseudo-devices below, then those drivers register as they attach
 * (before devfs is mounted, which makes a node for each).
 */

#include <stddef.h>

#include <sys/mkdev.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/systm.h>

extern struct streamtab ipinfo, strloopinfo, strmuxinfo, strpassinfo,
    strupinfo;

static struct fmodsw fmodsw[] = {
        { "strpass", &strpassinfo, SQLVL_QPAIR, NULL },
        { "strup", &strupinfo, SQLVL_MODULE, NULL },
};

struct strdev strdevsw[STRDEV_MAX] = {
        { "strloop", &strloopinfo, SQLVL_QPAIR, DEV_STRLOOP_MAJOR, 0666,
            NULL, 0, 0, NULL },
        { "strmux", &strmuxinfo, SQLVL_MODULE, DEV_STRMUX_MAJOR, 0666,
            NULL, 0, 0, NULL },
        { "ip", &ipinfo, SQLVL_MODULE, DEV_IP_MAJOR, 0666, NULL, 0, 0, NULL },
};
int nstrdev = 3;

struct fmodsw *
fmod_find(const char *name)
{
        for (size_t i = 0; i < sizeof(fmodsw) / sizeof(fmodsw[0]); i++)
                if (strcmp(fmodsw[i].f_name, name) == 0)
                        return &fmodsw[i];
        return NULL;
}

struct strdev *
strdev_find(const char *name)
{
        for (int i = 0; i < nstrdev; i++)
                if (strcmp(strdevsw[i].d_name, name) == 0)
                        return &strdevsw[i];
        return NULL;
}

/*
 * A driver's device, at attach: /dev/name, unit (0 to 255) of major,
 * whose open finds priv in d_private (by STR_UNIT of its dev_t). Boot
 * only, before devfs is mounted; NULL if the table is full.
 */
struct strdev *
strdev_register(const char *name, struct streamtab *st, int sqlvl,
    unsigned int major, unsigned int unit, mode_t mode, void *priv)
{
        struct strdev *d;

        if (nstrdev >= STRDEV_MAX || unit > 255 ||
            strlen(name) >= sizeof(d->d_name))
                return NULL;
        d = &strdevsw[nstrdev];
        memcpy(d->d_name, name, strlen(name) + 1);
        d->d_str = st;
        d->d_sqlvl = sqlvl;
        d->d_major = major;
        d->d_mode = mode;
        d->d_unit = unit;
        d->d_private = priv;
        nstrdev++;
        return d;
}
