/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/* The STREAMS modules that may be pushed, and the STREAMS devices. */

#include <stddef.h>

#include <sys/mkdev.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/systm.h>

extern struct streamtab strloopinfo, strpassinfo, strupinfo;

static struct fmodsw fmodsw[] = {
        { "strpass", &strpassinfo, SQLVL_QPAIR, NULL },
        { "strup", &strupinfo, SQLVL_MODULE, NULL },
};

struct strdev strdevsw[] = {
        { "strloop", &strloopinfo, SQLVL_QPAIR, DEV_STRLOOP_MAJOR, 0666,
            NULL, 0 },
};
const int nstrdev = sizeof(strdevsw) / sizeof(strdevsw[0]);

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
