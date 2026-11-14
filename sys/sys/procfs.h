/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * /proc, as IRIX keeps it: System V Release 4.0's flat file system, a
 * file per process, asked about with ioctl(). /proc/NNNNN (the process
 * id in decimal, five digits or more, with leading zeros) belongs to the
 * process's real user, mode 0600; /proc/pinfo/NNNNN, which anyone may
 * open, answers only PIOCPSINFO, so that ps(1) need not be privileged.
 * The ioctl numbers and field names are SVR4's; the layout is Belka's,
 * 64 bits wide.
 *
 * This header is shared with user programs and uses no kernel types.
 */

#ifndef _SYS_PROCFS_H_
#define _SYS_PROCFS_H_

#include <stdint.h>

#define PIOC            ('q' << 8)
#define PIOCPSINFO      (PIOC | 30)     /* struct prpsinfo */

#define PRNODEV         ((uint64_t)-1)  /* pr_ttydev: no controlling tty */
#define PRCLSZ          8
#define PRFNSZ          16
#define PRARGSZ         80

/* pr_flag */
#define PR_ISSYS        0x0100          /* a system process (process 0) */

struct prpsinfo {
        char            pr_state;       /* numeric state, KT_* plus 1 */
        char            pr_sname;       /* O R S T Z: the letter ps shows */
        char            pr_zomb;        /* 1 if a zombie */
        char            pr_nice;        /* 0 to 39; NZERO is 20 */
        uint32_t        pr_flag;        /* PR_* */
        uint32_t        pr_uid;         /* real user and group */
        uint32_t        pr_gid;
        uint32_t        pr_euid;        /* effective (Belka's addition) */
        uint32_t        pr_egid;
        int32_t         pr_pid;
        int32_t         pr_ppid;
        int32_t         pr_pgrp;
        int32_t         pr_sid;
        uint64_t        pr_addr;        /* where the process is; 0 */
        uint64_t        pr_size;        /* its address space, in pages */
        uint64_t        pr_rssize;      /* resident, in pages; 0 for now */
        uint64_t        pr_wchan;       /* what it waits on; 0 for now */
        int64_t         pr_start[2];    /* when it began: s, ns since 1970 */
        int64_t         pr_time[2];     /* processor time, user and system */
        int64_t         pr_ctime[2];    /* of its reaped children (IRIX) */
        int32_t         pr_pri;         /* priority, higher is sooner */
        int32_t         pr_oldpri;      /* 0 */
        int32_t         pr_cpu;         /* recent processor use; 0 */
        int32_t         pr_nlwp;        /* LWPs */
        uint64_t        pr_ttydev;      /* the controlling tty, or PRNODEV */
        char            pr_clname[PRCLSZ];      /* scheduling class */
        char            pr_fname[PRFNSZ];       /* the program's base name */
        char            pr_psargs[PRARGSZ];     /* its arguments, at exec */
        uint64_t        pr_spare[8];
};

#endif /* _SYS_PROCFS_H_ */
