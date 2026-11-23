/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for processes, from the kernel's side: start the user-mode
 * test programs that tools/mkext2.sh put in /bin of the 4 KB ext2 image,
 * and check how each one ends. utest checks the system calls from the
 * inside and exits with its number of failures.
 *
 * Several utests then run at once, twice. Afterwards every object a
 * process uses (process, LWP, open file, address space, segment, anon,
 * HAT) must be back to its count before the processes ran, pages in
 * use may have grown only by what kmem zones keep at a new high-water
 * mark, and the file system must unmount, which it cannot while a vnode
 * reference is leaked.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include "fs/ext2fs/ext2fs.h"

#include <sys/mman.h>

#include <machine/param.h>

#include <vm/anon.h>
#include <vm/as.h>
#include <vm/hat.h>
#include <vm/page.h>
#include <vm/seg_vn.h>

int selftest_proc(void);

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

#define NPARALLEL       6
#define PAGE_SLACK      64      /* zone growth, not a leak */

static const char *const proc_zones[] = {
        "proc", "lwp", "file", "as", "seg", "segvn", "anon", "anon_map",
        "hat", "cred"
};
#define NPROCZONES      (sizeof(proc_zones) / sizeof(proc_zones[0]))

/* Run path with argv; 1 if it ended with code and data. */
static int
run(const char *path, char *const argv[], int code, int data)
{
        char *envp[] = { "BELKA=1", NULL };
        int c = -1, d = -1;
        pid_t pid;

        if (proc_spawn(path, argv, envp, &pid) != 0)
                return 0;
        if (proc_wait(pid, &c, &d) != 0)
                return 0;
        if (c != code || d != data)
                printf("selftest: %s ended with %d/%d, not %d/%d\n", path,
                    c, d, code, data);
        return c == code && d == data;
}

/* Several utests at once; 1 if all of them passed. */
static int
run_parallel(void)
{
        char *argv[] = { "utest", "/p", NULL };
        char *sargv[] = { "sigtest", NULL };
        char *largv[] = { "lwptest", "/p/bin/argtest", NULL };
        static const char *const paths[] = {
                "/p/bin/utest", "/p/bin/sigtest", "/p/bin/lwptest"
        };
        char *const *argvs[] = { argv, sargv, largv };
        pid_t pids[NPARALLEL];
        int ok = 1;

        /* utests, sigtests, and lwptests side by side. */
        for (int i = 0; i < NPARALLEL; i++) {
                if (proc_spawn(paths[i % 3], argvs[i % 3], NULL,
                    &pids[i]) != 0) {
                        pids[i] = 0;
                        ok = 0;
                }
        }
        for (int i = 0; i < NPARALLEL; i++) {
                int c, d;

                if (pids[i] != 0 && (proc_wait(pids[i], &c, &d) != 0 ||
                    c != CLD_EXITED || d != 0))
                        ok = 0;
        }
        return ok;
}

/*
 * The address space layer from the kernel, with no process: faults,
 * unmapping a hole (a split), protection, and fork's copy on write, by
 * their effect on anon counts and translations.
 */
static void
test_vm(void)
{
        struct segvn_crargs a = { NULL, 0, MAP_PRIVATE,
            PROT_READ | PROT_WRITE, PROT_ALL };
        const vaddr_t b = UINT64_C(0x10000000);
        uint64_t base = anon_pages();
        struct as *as = as_alloc(), *cas;
        unsigned int attr = 0;
        int ok = 1;

        CHECK(as_map(as, b, 16 * PAGESIZE, segvn_create, &a) == 0);
        CHECK(as_map(as, b + PAGESIZE, PAGESIZE, segvn_create, &a) ==
            ENOMEM);
        for (int i = 0; i < 16; i++)
                ok &= as_fault(as, b + ptob(i), S_WRITE) == FC_OK;
        CHECK(ok);
        CHECK(anon_pages() - base == 16);
        CHECK(as_fault(as, b + ptob(16), S_READ) == FC_NOMAP);
        CHECK(as_fault(as, b, S_EXEC) == FC_PROT);

        /* Releasing one owner must preserve mappings for the survivor. */
        as_hold(as);
        as_free(as);
        CHECK(anon_pages() - base == 16);
        CHECK(as_fault(as, b + ptob(15), S_WRITE) == FC_OK);

        /* A hole in the middle splits the segment and frees its anons. */
        CHECK(as_unmap(as, b + ptob(4), ptob(4)) == 0);
        CHECK(anon_pages() - base == 12);
        CHECK(as->a_nsegs == 2);
        CHECK(as_fault(as, b + ptob(5), S_READ) == FC_NOMAP);
        CHECK(!hat_probe(as->a_hat, b + ptob(5), NULL, NULL));

        /* Read-only at the front: writes there are refused. */
        CHECK(as_setprot(as, b, ptob(2), PROT_READ) == 0);
        CHECK(as->a_nsegs == 3);
        CHECK(as_fault(as, b, S_WRITE) == FC_PROT);
        CHECK(as_fault(as, b + ptob(2), S_WRITE) == FC_OK);
        CHECK(as_setprot(as, b + ptob(3), ptob(2), PROT_READ) == ENOMEM);

        /* fork: shared anons, write-protected on both sides, then copied. */
        cas = as_dup(as);
        CHECK(anon_pages() - base == 12);
        CHECK(hat_probe(as->a_hat, b + ptob(2), NULL, &attr) &&
            (attr & HAT_WRITE) == 0);
        CHECK(as_fault(cas, b + ptob(2), S_WRITE) == FC_OK);
        CHECK(anon_pages() - base == 13);
        CHECK(as_fault(as, b + ptob(2), S_WRITE) == FC_OK);
        CHECK(anon_pages() - base == 13);       /* sole owner now */
        as_free(cas);
        CHECK(anon_pages() - base == 12);
        as_free(as);
        CHECK(anon_pages() == base);
}

static void
test_shared_fds(void)
{
        struct proc *p[3];
        struct vnode *vp;
        struct file *fp, *held;
        int fd, error;

        error = vn_open("/p/bin/hello", FREAD, 0, &vp);
        CHECK(error == 0);
        if (error != 0)
                return;
        for (int i = 0; i < 3; i++) {
                p[i] = kmem_zalloc(sizeof(*p[i]), KM_SLEEP);
                mutex_init(&p[i]->p_lock, "testproc");
                rlimit_init(p[i]);
                fd_init_table(p[i]);
        }
        fp = falloc(vp, FREAD);
        CHECK(fd_alloc(p[0], fp, 0, &fd) == 0);
        fd_share_table(p[0], p[1]);
        CHECK(fd_cloexec(p[1], fd, 1) == 1);
        CHECK(fd_cloexec(p[0], fd, -1) == 1);
        fhold(fp);
        CHECK(fd_install(p[1], fp, 200) == 0);
        held = fd_get(p[0], 200);
        CHECK(held == fp);
        if (held != NULL)
                frele(held);

        /* Exec detaches before acting on the shared close-on-exec flags. */
        fd_close_exec(p[1]);
        CHECK(fd_cloexec(p[1], fd, -1) == -1);
        CHECK(fd_cloexec(p[0], fd, -1) == 1);
        CHECK(fd_cloexec(p[1], 200, -1) == 0);
        fd_share_table(p[0], p[2]);
        fd_close_all(p[0]);
        CHECK(fd_cloexec(p[2], fd, -1) == 1);
        CHECK(fd_close(p[2], 200) == 0);
        CHECK(fd_cloexec(p[1], 200, -1) == 0);
        fd_close_all(p[2]);
        CHECK(fd_cloexec(p[1], 200, -1) == 0);
        fd_close_all(p[1]);
        for (int i = 0; i < 3; i++)
                kmem_free(p[i], sizeof(*p[i]));
}

/* A signal posted by the kernel ends a process that waits for one. */
static void
test_kernel_signal(void)
{
        char *argv[] = { "sigtest", "pause", NULL };
        struct proc *p;
        int c = -1, d = -1;
        pid_t pid;

        CHECK(proc_spawn("/p/bin/sigtest", argv, NULL, &pid) == 0);
        delay(HZ / 5);
        mutex_lock(&proctree_lock);
        if ((p = proc_find_locked(pid)) != NULL)
                psignal(p, SIGTERM);
        mutex_unlock(&proctree_lock);
        CHECK(p != NULL);
        CHECK(proc_wait(pid, &c, &d) == 0 && c == CLD_KILLED &&
            d == SIGTERM);
}

static size_t
pages_in_use(void)
{
        struct page_stats ps;

        page_get_stats(&ps);
        return ps.ps_managed - ps.ps_free - ps.ps_cached;
}

int
selftest_proc(void)
{
        struct ext2fs_args args = { "vd2", 0 };
        char *hello[] = { "hello", NULL };
        char *segv[] = { "segv", NULL };
        char *utest[] = { "utest", "/p", NULL };
        char *sigtest[] = { "sigtest", NULL };
        char *lwptest[] = { "lwptest", "/p/bin/argtest", NULL };
        char *libctest[] = { "libctest", "/p", NULL };
        char *threadtest[] = { "threadtest", NULL };
        char *dyntest[] = { "dyntest", NULL };
        char *dyntest_pie[] = { "dyntest-pie", NULL };
        char *credtest[] = { "credtest", "/p", NULL };
        char *ttytest[] = { "ttytest", NULL };
        char *ptytest[] = { "ptytest", NULL };
        char *sproctest[] = { "sproctest", "/p", NULL };
        char *strtest[] = { "strtest", NULL };
        char *nettest[] = { "nettest", NULL };
        char *iptest[] = { "iptest", NULL };
        char *udptest[] = { "udptest", NULL };
        size_t used1, used2, zone0[NPROCZONES];
        int zones_ok = 1;

        failures = 0;
        test_vm();
        for (size_t i = 0; i < NPROCZONES; i++)
                zone0[i] = kmem_zone_inuse_named(proc_zones[i]);
        CHECK(vn_mkdir("/p", 0755) == 0);
        CHECK(vfs_mount("ext2fs", "/p", &args) == 0);
        /* Dynamic programs find the shared C library in /usr/lib64. */
        CHECK(vn_mkdir("/usr", 0755) == 0);
        CHECK(vn_symlink("/p/usr/lib64", "/usr/lib64") == 0);
        /* The device names. */
        CHECK(vn_mkdir("/dev", 0755) == 0);
        CHECK(vfs_mount("devfs", "/dev", NULL) == 0);

        test_shared_fds();

        CHECK(run("/p/bin/hello", hello, CLD_EXITED, 0));
        CHECK(run("/p/bin/segv", segv, CLD_KILLED, SIGSEGV));
        CHECK(run("/p/bin/nothing", hello, CLD_EXITED, 127));
        CHECK(run("/p/bin/utest", utest, CLD_EXITED, 0));
        CHECK(run("/p/bin/sigtest", sigtest, CLD_EXITED, 0));
        CHECK(run("/p/bin/lwptest", lwptest, CLD_EXITED, 0));
        CHECK(run("/p/bin/libctest", libctest, CLD_EXITED, 0));
        CHECK(run("/p/bin/libctest-static", libctest, CLD_EXITED, 0));
        CHECK(run("/p/bin/threadtest", threadtest, CLD_EXITED, 0));
        CHECK(run("/p/bin/dyntest", dyntest, CLD_EXITED, 0));
        CHECK(run("/p/bin/dyntest-pie", dyntest_pie, CLD_EXITED, 0));
        CHECK(run("/p/bin/credtest", credtest, CLD_EXITED, 0));
        CHECK(run("/p/bin/ttytest", ttytest, CLD_EXITED, 0));
        CHECK(run("/p/bin/ptytest", ptytest, CLD_EXITED, 0));
        CHECK(run("/p/bin/strtest", strtest, CLD_EXITED, 0));
        CHECK(run("/p/bin/nettest", nettest, CLD_EXITED, 0));
        CHECK(run("/p/bin/iptest", iptest, CLD_EXITED, 0));
        CHECK(run("/p/bin/udptest", udptest, CLD_EXITED, 0));
        CHECK(run("/p/bin/sproctest", sproctest, CLD_EXITED, 0));
        CHECK(run("/p/bin/sproctest-static", sproctest, CLD_EXITED, 0));
        test_kernel_signal();
        CHECK(proc_wait(12345, &(int){ 0 }, &(int){ 0 }) == ECHILD);

        CHECK(run_parallel());
        used1 = pages_in_use();
        CHECK(run_parallel());
        used2 = pages_in_use();
        printf("selftest: processes: %zu pages in use after one batch, %zu "
            "after another\n", used1, used2);
        CHECK(used2 <= used1 + PAGE_SLACK);
        CHECK(proc_count() == 0);
        CHECK(anon_pages() == 0);
        for (size_t i = 0; i < NPROCZONES; i++) {
                size_t n = kmem_zone_inuse_named(proc_zones[i]);

                if (n != zone0[i]) {
                        printf("selftest: zone %s: %zu in use, was %zu\n",
                            proc_zones[i], n, zone0[i]);
                        zones_ok = 0;
                }
        }
        CHECK(zones_ok);

        CHECK(vn_remove("/usr/lib64") == 0);
        CHECK(vn_rmdir("/usr") == 0);
        CHECK(vfs_unmount("/p") == 0);
        CHECK(vn_rmdir("/p") == 0);
        return failures;
}
