/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Credentials, after SVR4: real, effective, and saved user and group
 * ids, and supplementary groups. A cred is shared by reference and
 * never changed once others can see it; a change makes a new one
 * (crdup) and puts it in the process (cred_setproc).
 *
 * A process's p_cred is replaced only by the process itself, under
 * p_lock, and the pointer is swapped under p_crlock as well, so another
 * process (kill) may take a reference under p_crlock alone. Each LWP
 * keeps its own reference, l_cred, brought up to date on entry to every
 * system call; CRED() is that, so system call code uses one set of
 * credentials throughout without locking. Kernel threads use kcred.
 */

#ifndef _SYS_CRED_H_
#define _SYS_CRED_H_

#include <stdint.h>

#include <sys/types.h>

#define NGROUPS_MAX     16      /* supplementary groups, as in UnixWare */

struct cred {
        volatile uint32_t cr_ref;
        uid_t           cr_uid;         /* effective */
        gid_t           cr_gid;
        uid_t           cr_ruid;        /* real */
        gid_t           cr_rgid;
        uid_t           cr_suid;        /* saved */
        gid_t           cr_sgid;
        int             cr_ngroups;
        gid_t           cr_groups[NGROUPS_MAX];
};

/* Credentials of the kernel itself: the superuser. */
extern struct cred *kcred;

/* Privilege is an effective user id of 0. */
#define CRED_ISROOT(cr) ((cr)->cr_uid == 0)

struct lwp;
struct proc;

void            cred_init(void);
void            crhold(struct cred *);
void            crfree(struct cred *);
struct cred     *crdup(const struct cred *);
int             groupmember(gid_t, const struct cred *);
struct cred     *cred_getproc(struct proc *);
void            cred_setproc(struct proc *, struct cred *);
void            cred_refresh(struct lwp *);
struct cred     *cred_get(void);

/* The calling LWP's credentials, or the kernel's. */
#define CRED()          (cred_get())

#endif /* _SYS_CRED_H_ */
