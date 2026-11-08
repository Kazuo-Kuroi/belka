/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The native system call table, and the calls too small for a file of
 * their own.
 *
 * Each handler gets the six argument registers and two return values,
 * preset to 0 and the caller's %rdx (as in UnixWare), and returns 0, an
 * errno value, or EJUSTRETURN if it has set the registers itself. A
 * number with no handler fails with EINVAL; UnixWare also posts SIGSYS,
 * which comes with signals.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/syscall.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#include <machine/md_var.h>

static int
sys_getpid(const uint64_t *args __unused, uint64_t *rv)
{
        struct proc *p = curproc;

        rv[0] = (uint64_t)p->p_pid;
        mutex_lock(&proctree_lock);
        rv[1] = (uint64_t)p->p_parent->p_pid;
        mutex_unlock(&proctree_lock);
        return 0;
}

/* Real and effective ids; there are no others yet. */
static int
sys_getuid(const uint64_t *args __unused, uint64_t *rv)
{
        rv[0] = rv[1] = curproc->p_cred->cr_uid;
        return 0;
}

static int
sys_getgid(const uint64_t *args __unused, uint64_t *rv)
{
        rv[0] = rv[1] = curproc->p_cred->cr_gid;
        return 0;
}

static int
sys_time(const uint64_t *args __unused, uint64_t *rv)
{
        struct timespec ts;

        gethrestime(&ts);
        rv[0] = (uint64_t)ts.tv_sec;
        return 0;
}

/* gettimeofday(struct timeval *): seconds and microseconds. */
static int
sys_gettimeofday(const uint64_t *args, uint64_t *rv __unused)
{
        struct timespec ts;
        int64_t tv[2];

        gethrestime(&ts);
        tv[0] = ts.tv_sec;
        tv[1] = ts.tv_nsec / 1000;
        return args[0] != 0 ? copyout(tv, (void *)(uintptr_t)args[0],
            sizeof(tv)) : 0;
}

static int
sys_uname(const uint64_t *args, uint64_t *rv __unused)
{
        static struct utsname u;

        if (u.sysname[0] == '\0') {
                memcpy(u.sysname, "Belka", 6);
                memcpy(u.nodename, "belka", 6);
                memcpy(u.release, "0.0", 4);
                memcpy(u.version, "dev", 4);
                memcpy(u.machine, "amd64", 6);
        }
        return copyout(&u, (void *)(uintptr_t)args[0], sizeof(u));
}

/* __lwp_private(void *): the LWP private data pointer, the %fs base. */
static int
sys_lwpprivate(const uint64_t *args, uint64_t *rv __unused)
{
        md_set_private(curlwp, args[0]);
        return 0;
}

static int
sys_sync(const uint64_t *args __unused, uint64_t *rv __unused)
{
        vfs_syncall();
        return 0;
}

static int
sys_umask(const uint64_t *args, uint64_t *rv)
{
        struct proc *p = curproc;

        mutex_lock(&p->p_lock);
        rv[0] = p->p_cmask;
        p->p_cmask = (mode_t)args[0] & 0777;
        mutex_unlock(&p->p_lock);
        return 0;
}

static sy_call_t *const sysent[NSYSCALL] = {
        [SYS_exit] = sys_exit,
        [SYS_fork] = sys_fork,
        [SYS_read] = sys_read,
        [SYS_write] = sys_write,
        [SYS_open] = sys_open,
        [SYS_close] = sys_close,
        [SYS_wait] = sys_wait,
        [SYS_creat] = sys_creat,
        [SYS_link] = sys_link,
        [SYS_unlink] = sys_unlink,
        [SYS_exec] = sys_exec,
        [SYS_chdir] = sys_chdir,
        [SYS_time] = sys_time,
        [SYS_chmod] = sys_chmod,
        [SYS_brk] = sys_brk,
        [SYS_stat] = sys_stat,
        [SYS_lseek] = sys_lseek,
        [SYS_getpid] = sys_getpid,
        [SYS_getuid] = sys_getuid,
        [SYS_fstat] = sys_fstat,
        [SYS_access] = sys_access,
        [SYS_sync] = sys_sync,
        [SYS_dup] = sys_dup,
        [SYS_getgid] = sys_getgid,
        [SYS_ioctl] = sys_ioctl,
        [SYS_execve] = sys_execve,
        [SYS_umask] = sys_umask,
        [SYS_fcntl] = sys_fcntl,
        [SYS_rmdir] = sys_rmdir,
        [SYS_mkdir] = sys_mkdir,
        [SYS_getdents] = sys_getdents,
        [SYS_lstat] = sys_lstat,
        [SYS_symlink] = sys_symlink,
        [SYS_readlink] = sys_readlink,
        [SYS_waitsys] = sys_waitsys,
        [SYS_mmap] = sys_mmap,
        [SYS_mprotect] = sys_mprotect,
        [SYS_munmap] = sys_munmap,
        [SYS_fchdir] = sys_fchdir,
        [SYS_rename] = sys_rename,
        [SYS_uname] = sys_uname,
        [SYS_gettimeofday] = sys_gettimeofday,
        [SYS_lwpprivate] = sys_lwpprivate,
        [SYS_pread] = sys_pread,
        [SYS_pwrite] = sys_pwrite,
        [SYS_ftruncate] = sys_ftruncate,
        [SYS_fork1] = sys_fork,
};

int
syscall_dispatch(uint64_t code, const uint64_t *args, uint64_t *rv)
{
        if (code >= NSYSCALL || sysent[code] == NULL)
                return EINVAL;
        return sysent[code](args, rv);
}
