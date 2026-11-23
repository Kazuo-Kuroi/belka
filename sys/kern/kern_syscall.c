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
 * number with no handler posts SIGSYS to the caller and fails with
 * EINVAL, as in UnixWare; 127, UnixWare's invsys, only fails.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/prctl.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/sysent.h>
#include <sys/systm.h>
#include <sys/spinlock.h>
#include <sys/systeminfo.h>
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

/*
 * The names uname and sysinfo report; the node name is the host name,
 * which sysinfo(SI_SET_HOSTNAME) changes, under uts_lock.
 */
static struct utsname uts = {
        "Belka", "belka", "0.0", "dev", "amd64"
};
static struct spinlock uts_lock = SPINLOCK_INITIALIZER("uts");

static int
sys_uname(const uint64_t *args, uint64_t *rv __unused)
{
        struct utsname u;
        int s;

        s = spin_lock(&uts_lock);
        u = uts;
        spin_unlock(&uts_lock, s);
        return copyout(&u, (void *)(uintptr_t)args[0], sizeof(u));
}

/* sysinfo(command, buf, count): see <sys/systeminfo.h>. */
static int
sys_systeminfo(const uint64_t *args, uint64_t *rv)
{
        int cmd = (int)args[0];
        size_t count = (size_t)args[2], len;
        char name[SYS_NMLN];
        int s, error;

        if (cmd == SI_SET_HOSTNAME) {
                if (!CRED_ISROOT(CRED()))
                        return EPERM;
                if ((error = copyinstr((const char *)(uintptr_t)args[1], name,
                    sizeof(name), &len)) != 0)
                        return error == ENAMETOOLONG ? EINVAL : error;
                s = spin_lock(&uts_lock);
                memcpy(uts.nodename, name, len);
                spin_unlock(&uts_lock, s);
                rv[0] = len;
                return 0;
        }
        s = spin_lock(&uts_lock);
        switch (cmd) {
        case SI_SYSNAME:
                memcpy(name, uts.sysname, sizeof(name));
                break;
        case SI_HOSTNAME:
                memcpy(name, uts.nodename, sizeof(name));
                break;
        case SI_RELEASE:
                memcpy(name, uts.release, sizeof(name));
                break;
        case SI_VERSION:
                memcpy(name, uts.version, sizeof(name));
                break;
        case SI_MACHINE:
        case SI_ARCHITECTURE:
                memcpy(name, uts.machine, sizeof(name));
                break;
        default:
                spin_unlock(&uts_lock, s);
                return EINVAL;
        }
        spin_unlock(&uts_lock, s);
        len = strlen(name) + 1;
        if (count > 0) {
                size_t n = len < count ? len : count;

                name[n - 1] = '\0';
                if ((error = copyout(name, (void *)(uintptr_t)args[1],
                    n)) != 0)
                        return error;
        }
        rv[0] = len;
        return 0;
}

/* __lwp_private(void *): the LWP private data pointer, the %fs base. */
static int
sys_lwpprivate(const uint64_t *args, uint64_t *rv __unused)
{
        return md_set_private(curlwp, args[0]);
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
        [SYS_sproc] = sys_sproc,
        [SYS_sprocsp] = sys_sprocsp,
        [SYS_prctl] = sys_prctl,
        [SYS_procblk] = sys_procblk,
        [SYS_getpmsg] = sys_getpmsg,
        [SYS_putpmsg] = sys_putpmsg,
        [SYS_sendto] = sys_sendto,
        [SYS_recvfrom] = sys_recvfrom,
        [SYS_getsockname] = sys_getsockname,
        [SYS_getpeername] = sys_getpeername,
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
        [SYS_chown] = sys_chown,
        [SYS_lchown] = sys_lchown,
        [SYS_setrlimit] = sys_setrlimit,
        [SYS_getrlimit] = sys_getrlimit,
        [SYS_brk] = sys_brk,
        [SYS_stat] = sys_stat,
        [SYS_lseek] = sys_lseek,
        [SYS_getpid] = sys_getpid,
        [SYS_getuid] = sys_getuid,
        [SYS_stime] = sys_stime,
        [SYS_utime] = sys_utime,
        [SYS_nice] = sys_nice,
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
        [SYS_mknod] = sys_mknod,
        [SYS_getdents] = sys_getdents,
        [SYS_lstat] = sys_lstat,
        [SYS_symlink] = sys_symlink,
        [SYS_readlink] = sys_readlink,
        [SYS_statvfs] = sys_statvfs,
        [SYS_fstatvfs] = sys_fstatvfs,
        [SYS_waitsys] = sys_waitsys,
        [SYS_mmap] = sys_mmap,
        [SYS_mprotect] = sys_mprotect,
        [SYS_munmap] = sys_munmap,
        [SYS_fchdir] = sys_fchdir,
        [SYS_rename] = sys_rename,
        [SYS_uname] = sys_uname,
        [SYS_systeminfo] = sys_systeminfo,
        [SYS_setitimer] = sys_setitimer,
        [SYS_getitimer] = sys_getitimer,
        [SYS_gettimeofday] = sys_gettimeofday,
        [SYS_lwpprivate] = sys_lwpprivate,
        [SYS_pread] = sys_pread,
        [SYS_pwrite] = sys_pwrite,
        [SYS_ftruncate] = sys_ftruncate,
        [SYS_fork1] = sys_fork1,
        [SYS_forkall] = sys_fork,
        [SYS_lwpcreate] = sys_lwpcreate,
        [SYS_lwpexit] = sys_lwpexit,
        [SYS_lwpwait] = sys_lwpwait,
        [SYS_lwpself] = sys_lwpself,
        [SYS_lwpinfo] = sys_lwpinfo,
        [SYS_lwpkill] = sys_lwpkill,
        [SYS_sigwait] = sys_sigwait,
        [SYS_lwpsuspend] = sys_lwpsuspend,
        [SYS_lwpcontinue] = sys_lwpcontinue,
        [SYS_lwp_sema_wait] = sys_lwp_sema_wait,
        [SYS_lwp_sema_post] = sys_lwp_sema_post,
        [SYS_lwp_sema_trywait] = sys_lwp_sema_trywait,
        [SYS_lwp_addr_wait] = sys_lwp_addr_wait,
        [SYS_lwp_addr_wake] = sys_lwp_addr_wake,
        [SYS_clock_gettime] = sys_clock_gettime,
        [SYS_clock_getres] = sys_clock_getres,
        [SYS_times] = sys_times,
        [SYS_pipe] = sys_pipe,
        [SYS_getmsg] = sys_getmsg,
        [SYS_putmsg] = sys_putmsg,
        [SYS_poll] = sys_poll,
        [SYS_uadmin] = sys_uadmin,
        [SYS_fsync] = sys_fsync,
        [SYS_fchmod] = sys_fchmod,
        [SYS_fchown] = sys_fchown,
        [SYS_readv] = sys_readv,
        [SYS_writev] = sys_writev,
        [SYS_yield] = sys_yield,
        [SYS_openat] = sys_openat,
        [SYS_mknodat] = sys_mknodat,
        [SYS_mkdirat] = sys_mkdirat,
        [SYS_unlinkat] = sys_unlinkat,
        [SYS_renameat] = sys_renameat,
        [SYS_linkat] = sys_linkat,
        [SYS_symlinkat] = sys_symlinkat,
        [SYS_readlinkat] = sys_readlinkat,
        [SYS_fstatat] = sys_fstatat,
        [SYS_fchownat] = sys_fchownat,
        [SYS_fchmodat] = sys_fchmodat,
        [SYS_faccessat] = sys_faccessat,
        [SYS_pollsys] = sys_pollsys,
        [SYS_getrusage] = sys_getrusage,
        [SYS_utimensat] = sys_utimensat,
        [SYS_flock] = sys_flock,
        [SYS_mount] = sys_mount,
        [SYS_umount] = sys_umount,
        [SYS_setuid] = sys_setuid,
        [SYS_seteuid] = sys_seteuid,
        [SYS_setgid] = sys_setgid,
        [SYS_setegid] = sys_setegid,
        [SYS_getgroups] = sys_getgroups,
        [SYS_setgroups] = sys_setgroups,
        [SYS_alarm] = sys_alarm,
        [SYS_pause] = sys_pause,
        [SYS_kill] = sys_kill,
        [SYS_sigqueue] = sys_sigqueue,
        [SYS_socketpair] = sys_socketpair,
        [SYS_send] = sys_send,
        [SYS_recv] = sys_recv,
        [SYS_sockatmark] = sys_sockatmark,
        [SYS_pipe2] = sys_pipe2,
        [SYS_socket] = sys_socket,
        [SYS_bind] = sys_bind,
        [SYS_listen] = sys_listen,
        [SYS_connect] = sys_connect,
        [SYS_accept] = sys_accept,
        [SYS_aio_notify] = sys_aio_notify,
        [SYS_aio_dup] = sys_aio_dup,
        [SYS_pgrpsys] = sys_pgrpsys,
        [SYS_signal] = sys_signal,
        [SYS_sigprocmask] = sys_sigprocmask,
        [SYS_sigsuspend] = sys_sigsuspend,
        [SYS_sigaltstack] = sys_sigaltstack,
        [SYS_sigaction] = sys_sigaction,
        [SYS_sigpending] = sys_sigpending,
        [SYS_context] = sys_context,
};

#define SYS_invsys      127

int
syscall_dispatch(uint64_t code, const uint64_t *args, uint64_t *rv)
{
        int error;
        unsigned int changed = 0;

        sproc_sync(curproc);
        cred_refresh(curlwp);
        if (code >= NSYSCALL || sysent[code] == NULL) {
                if (code != SYS_invsys)
                        psignal(curproc, SIGSYS);
                return EINVAL;
        }
        error = sysent[code](args, rv);
        if (error == 0) {
                switch (code) {
                case SYS_chdir:
                case SYS_fchdir:
                        changed = PR_SDIR;
                        break;
                case SYS_umask:
                        changed = PR_SUMASK;
                        break;
                case SYS_setrlimit:
                        if (args[0] == RLIMIT_FSIZE)
                                changed = PR_SULIMIT;
                        break;
                case SYS_setuid:
                case SYS_seteuid:
                case SYS_setgid:
                case SYS_setegid:
                case SYS_setgroups:
                        changed = PR_SID;
                        break;
                }
                if (changed != 0)
                        sproc_publish(curproc, changed);
        }
        return error;
}
