/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Credentials. Only a user and a group for now; supplementary groups
 * and privileges come with processes.
 */

#ifndef _SYS_CRED_H_
#define _SYS_CRED_H_

#include <stdint.h>

#include <sys/types.h>

struct cred {
        uid_t           cr_uid;
        gid_t           cr_gid;
};

/* Credentials of the kernel itself: the superuser. */
extern struct cred *kcred;

#define CRED_ISROOT(cr) ((cr)->cr_uid == 0)

#endif /* _SYS_CRED_H_ */
