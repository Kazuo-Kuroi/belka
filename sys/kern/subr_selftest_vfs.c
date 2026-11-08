/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for the VFS layer, pathname lookup, the name cache, and
 * tmpfs, through the kernel file API. Everything is created under
 * /selftest and removed again, and tmpfs must end with the pages and
 * nodes it started with.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/dirent.h>
#include <sys/dnlc.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/pathname.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/param.h>

#include "fs/tmpfs/tmpfs.h"

int selftest_vfs(void);

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

static int
put(const char *path, const void *buf, size_t len, off_t off)
{
        struct vnode *vp;
        size_t resid;
        int error;

        if ((error = vn_open(path, FWRITE | FCREAT, 0644, &vp)) != 0)
                return error;
        error = vn_rdwr(UIO_WRITE, vp, (void *)(uintptr_t)buf, len, off, 0,
            &resid);
        (void)vn_close(vp, FWRITE);
        if (error == 0 && resid != 0)
                error = ENOSPC;
        return error;
}

/* Read up to len bytes; returns bytes read, or -errno. */
static long
get(const char *path, void *buf, size_t len, off_t off)
{
        struct vnode *vp;
        size_t resid;
        int error;

        if ((error = vn_open(path, FREAD, 0, &vp)) != 0)
                return -error;
        error = vn_rdwr(UIO_READ, vp, buf, len, off, 0, &resid);
        (void)vn_close(vp, FREAD);
        return error != 0 ? -error : (long)(len - resid);
}

static struct vnode *
look(const char *path, int follow)
{
        struct vnode *vp;

        return lookupname(path, follow, &vp) == 0 ? vp : NULL;
}

static off_t
size_of(const char *path)
{
        struct vattr va;

        return vn_stat(path, FOLLOW, &va) == 0 ? va.va_size : -1;
}

static nlink_t
links_of(const char *path)
{
        struct vattr va;

        return vn_stat(path, NO_FOLLOW, &va) == 0 ? va.va_nlink : 0;
}

static void
test_lookup_rules(void)
{
        char longname[300];
        struct vnode *a, *b;

        CHECK(vn_mkdir("/selftest", 0755) == 0);
        CHECK(vn_mkdir("/selftest/a", 0755) == 0);
        CHECK(vn_mkdir("/selftest/a/b", 0755) == 0);
        CHECK(put("/selftest/a/b/f", "hello", 5, 0) == 0);

        a = look("/", FOLLOW);
        CHECK(a == rootdir && (a->v_flag & VROOT) != 0);
        if (a != NULL)
                VN_RELE(a);
        a = look("/selftest/a/b/f", FOLLOW);
        b = look("//selftest//a/./b/../b///f", FOLLOW);
        CHECK(a != NULL && a == b);
        if (b != NULL)
                VN_RELE(b);
        b = look("/selftest/a/b/f/", FOLLOW);           /* trailing slash */
        CHECK(a != NULL && a == b);
        if (b != NULL)
                VN_RELE(b);
        if (a != NULL)
                VN_RELE(a);
        a = look("/..", FOLLOW);                        /* stays at root */
        CHECK(a == rootdir);
        if (a != NULL)
                VN_RELE(a);

        CHECK(lookupname("", FOLLOW, &a) == ENOENT);
        CHECK(lookupname("/selftest/nope", FOLLOW, &a) == ENOENT);
        CHECK(lookupname("/selftest/a/b/f/x", FOLLOW, &a) == ENOTDIR);
        memset(longname, 'x', sizeof(longname));
        memcpy(longname, "/selftest/", 10);
        longname[sizeof(longname) - 1] = '\0';
        CHECK(lookupname(longname, FOLLOW, &a) == ENAMETOOLONG);
        CHECK(vn_mkdir("/", 0755) == EINVAL);           /* parent of / */
        CHECK(vn_rmdir("/selftest/a/.") == EINVAL);
        CHECK(vn_mkdir("/selftest/a", 0755) == EEXIST);
        CHECK(vn_rmdir("/selftest/a") == ENOTEMPTY);
        CHECK(vn_rmdir("/selftest/a/b/f") == ENOTDIR);
        CHECK(vn_remove("/selftest/a") == EPERM);
}

static void
test_file_data(void)
{
        unsigned char *buf = kmem_alloc(3 * PAGESIZE, KM_SLEEP);
        unsigned char *chk = kmem_alloc(3 * PAGESIZE, KM_SLEEP);
        struct vnode *vp;
        int holes_ok = 1;

        for (size_t i = 0; i < 3 * PAGESIZE; i++)
                buf[i] = (unsigned char)(i * 7 + 3);
        /* Unaligned, spanning pages. */
        CHECK(put("/selftest/data", buf, 10000, 100) == 0);
        CHECK(size_of("/selftest/data") == 10100);
        CHECK(get("/selftest/data", chk, 10000, 100) == 10000);
        CHECK(memcmp(buf, chk, 10000) == 0);
        CHECK(get("/selftest/data", chk, 100, 0) == 100);
        for (int i = 0; i < 100; i++)
                holes_ok &= chk[i] == 0;
        CHECK(holes_ok);
        CHECK(get("/selftest/data", chk, 500, 10050) == 50);   /* at EOF */
        CHECK(get("/selftest/data", chk, 10, 20000) == 0);

        /* A sparse write far out, and a hole read in between. */
        CHECK(put("/selftest/data", "end", 3, 1 << 20) == 0);
        CHECK(size_of("/selftest/data") == (1 << 20) + 3);
        CHECK(get("/selftest/data", chk, 64, 500000) == 64);
        holes_ok = 1;
        for (int i = 0; i < 64; i++)
                holes_ok &= chk[i] == 0;
        CHECK(holes_ok);

        /* Truncate into a page, then extend: the tail must read zero. */
        CHECK(vn_truncate("/selftest/data", 5000) == 0);
        CHECK(size_of("/selftest/data") == 5000);
        CHECK(vn_truncate("/selftest/data", 6000) == 0);
        CHECK(get("/selftest/data", chk, 1000, 5000) == 1000);
        holes_ok = 1;
        for (int i = 0; i < 1000; i++)
                holes_ok &= chk[i] == 0;
        CHECK(holes_ok);

        /* Open flags. */
        CHECK(vn_open("/selftest/data", FREAD | FWRITE | FCREAT | FEXCL, 0644,
            &vp) == EEXIST);
        CHECK(vn_open("/selftest/data", FWRITE | FTRUNC, 0, &vp) == 0);
        if (vp != NULL)
                (void)vn_close(vp, FWRITE);
        CHECK(size_of("/selftest/data") == 0);
        CHECK(vn_open("/selftest/a", FWRITE, 0, &vp) == EISDIR);
        CHECK(vn_open("/selftest/missing", FREAD, 0, &vp) == ENOENT);
        CHECK(vn_remove("/selftest/data") == 0);
        CHECK(vn_remove("/selftest/data") == ENOENT);
        kmem_free(buf, 3 * PAGESIZE);
        kmem_free(chk, 3 * PAGESIZE);
}

static void
test_links_and_rename(void)
{
        char buf[16];

        CHECK(vn_link("/selftest/a/b/f", "/selftest/h") == 0);
        CHECK(links_of("/selftest/a/b/f") == 2);
        CHECK(vn_link("/selftest/a/b/f", "/selftest/h") == EEXIST);
        CHECK(vn_link("/selftest/a", "/selftest/dirlink") == EPERM);
        CHECK(vn_remove("/selftest/a/b/f") == 0);
        CHECK(links_of("/selftest/h") == 1);
        CHECK(get("/selftest/h", buf, 16, 0) == 5 && memcmp(buf, "hello", 5) == 0);

        /* Rename within and across directories, and over a file. */
        CHECK(vn_rename("/selftest/h", "/selftest/h2") == 0);
        CHECK(get("/selftest/h", buf, 16, 0) == -ENOENT);
        CHECK(vn_rename("/selftest/h2", "/selftest/a/b/f") == 0);
        CHECK(put("/selftest/other", "x", 1, 0) == 0);
        CHECK(vn_rename("/selftest/other", "/selftest/a/b/f") == 0);
        CHECK(get("/selftest/a/b/f", buf, 16, 0) == 1 && buf[0] == 'x');
        CHECK(vn_rename("/selftest/a/b/f", "/selftest/a/b/f") == 0);

        /* Directories. */
        CHECK(vn_mkdir("/selftest/d1", 0755) == 0);
        CHECK(vn_mkdir("/selftest/d1/sub", 0755) == 0);
        CHECK(links_of("/selftest/d1") == 3);
        CHECK(vn_rename("/selftest/d1", "/selftest/d1/sub/x") == EINVAL);
        CHECK(vn_rename("/selftest/d1", "/selftest/a/b/f") == ENOTDIR);
        CHECK(vn_rename("/selftest/a/b/f", "/selftest/d1") == EISDIR);
        CHECK(vn_rename("/selftest/d1/sub", "/selftest/a/sub") == 0);
        CHECK(links_of("/selftest/d1") == 2 && links_of("/selftest/a") == 4);
        {
                struct vnode *p = look("/selftest/a/sub/..", FOLLOW);
                struct vnode *q = look("/selftest/a", FOLLOW);

                CHECK(p != NULL && p == q);
                if (p != NULL)
                        VN_RELE(p);
                if (q != NULL)
                        VN_RELE(q);
        }
        CHECK(vn_rename("/selftest/d1", "/selftest/a/sub") == 0); /* empty */
        CHECK(vn_rmdir("/selftest/a/sub") == 0);
}

static void
test_symlinks(void)
{
        char text[MAXPATHLEN];
        struct vnode *a, *b;
        size_t len;

        CHECK(vn_symlink("b/f", "/selftest/a/rel") == 0);
        CHECK(vn_symlink("/selftest/a/b", "/selftest/abs") == 0);
        CHECK(vn_symlink("/selftest/loop2", "/selftest/loop1") == 0);
        CHECK(vn_symlink("/selftest/loop1", "/selftest/loop2") == 0);
        CHECK(vn_symlink("nowhere", "/selftest/dangling") == 0);
        CHECK(vn_symlink("x", "/selftest/abs") == EEXIST);

        a = look("/selftest/a/rel", FOLLOW);
        b = look("/selftest/a/b/f", FOLLOW);
        CHECK(a != NULL && a == b);
        if (a != NULL)
                VN_RELE(a);
        a = look("/selftest/abs/f", FOLLOW);            /* mid-path link */
        CHECK(a != NULL && a == b);
        if (a != NULL)
                VN_RELE(a);
        if (b != NULL)
                VN_RELE(b);
        a = look("/selftest/a/rel", NO_FOLLOW);
        CHECK(a != NULL && a->v_type == VLNK);
        if (a != NULL)
                VN_RELE(a);
        CHECK(vn_readlink("/selftest/a/rel", text, sizeof(text), &len) == 0 &&
            len == 3 && strcmp(text, "b/f") == 0);
        CHECK(lookupname("/selftest/loop1", FOLLOW, &a) == ELOOP);
        CHECK(lookupname("/selftest/dangling", FOLLOW, &a) == ENOENT);
        /* Creating through a dangling link creates its target. */
        CHECK(put("/selftest/dangling", "d", 1, 0) == 0);
        CHECK(get("/selftest/nowhere", text, 4, 0) == 1);

        CHECK(vn_remove("/selftest/a/rel") == 0);
        CHECK(vn_remove("/selftest/abs") == 0);
        CHECK(vn_remove("/selftest/loop1") == 0);
        CHECK(vn_remove("/selftest/loop2") == 0);
        CHECK(vn_remove("/selftest/dangling") == 0);
        CHECK(vn_remove("/selftest/nowhere") == 0);
}

static void
test_readdir(void)
{
        static const char *const names[] = { "one", "two", "three" };
        char *buf = kmem_alloc(4096, KM_SLEEP);
        struct vnode *vp;
        int seen = 0, dots = 0, eof = 0, records = 0;
        struct iovec iov;
        struct uio uio;

        CHECK(vn_mkdir("/selftest/rd", 0755) == 0);
        for (int i = 0; i < 3; i++) {
                char path[64];

                snprintf(path, sizeof(path), "/selftest/rd/%s", names[i]);
                CHECK(put(path, "", 0, 0) == 0);
        }
        vp = look("/selftest/rd", FOLLOW);
        CHECK(vp != NULL);
        if (vp == NULL)
                goto out;

        /* Read with a small buffer to exercise resuming at an offset. */
        uio.uio_offset = 0;
        while (!eof && records < 20) {
                size_t used, pos = 0;

                iov.iov_base = buf;
                iov.iov_len = 48;
                uio.uio_iov = &iov;
                uio.uio_iovcnt = 1;
                uio.uio_resid = 48;
                uio.uio_segflg = UIO_SYSSPACE;
                uio.uio_rw = UIO_READ;
                CHECK(VOP_READDIR(vp, &uio, &eof, kcred) == 0);
                used = 48 - (size_t)uio.uio_resid;
                while (pos < used) {
                        struct dirent *d = (struct dirent *)(void *)(buf + pos);

                        if (strcmp(d->d_name, ".") == 0 ||
                            strcmp(d->d_name, "..") == 0)
                                dots++;
                        for (int i = 0; i < 3; i++)
                                seen += strcmp(d->d_name, names[i]) == 0;
                        pos += d->d_reclen;
                        records++;
                }
                if (used == 0 && !eof)
                        break;
        }
        VN_RELE(vp);
        CHECK(eof && dots == 2 && seen == 3 && records == 5);
        for (int i = 0; i < 3; i++) {
                char path[64];

                snprintf(path, sizeof(path), "/selftest/rd/%s", names[i]);
                CHECK(vn_remove(path) == 0);
        }
        CHECK(vn_rmdir("/selftest/rd") == 0);
out:
        kmem_free(buf, 4096);
}

static void
test_mounts(void)
{
        struct tmpfs_args args = { 16 };        /* 64 KB of file pages */
        unsigned char *big = kmem_alloc(128 * 1024, KM_SLEEP);
        struct statvfs sv;
        struct vnode *vp, *up;
        char buf[8];
        size_t resid;

        CHECK(vn_mkdir("/selftest/mnt", 0755) == 0);
        CHECK(put("/selftest/mnt/hidden", "h", 1, 0) == 0);
        CHECK(vfs_mount("tmpfs", "/selftest/mnt", &args) == 0);
        CHECK(vfs_mount("tmpfs", "/selftest/mnt", &args) == EBUSY);
        CHECK(get("/selftest/mnt/hidden", buf, 8, 0) == -ENOENT);
        CHECK(put("/selftest/mnt/inside", "i", 1, 0) == 0);

        vp = look("/selftest/mnt", FOLLOW);
        CHECK(vp != NULL && (vp->v_flag & VROOT) != 0 && vp != rootdir);
        up = look("/selftest/mnt/..", FOLLOW);  /* crosses back out */
        {
                struct vnode *st = look("/selftest", FOLLOW);

                CHECK(up != NULL && up == st);
                if (st != NULL)
                        VN_RELE(st);
        }
        if (up != NULL)
                VN_RELE(up);

        CHECK(vn_link("/selftest/mnt/inside", "/selftest/x") == EXDEV);
        CHECK(vn_rename("/selftest/mnt/inside", "/selftest/x") == EXDEV);
        CHECK(vn_rmdir("/selftest/mnt") == EBUSY);

        /* A full file system gives a short write, then ENOSPC. */
        CHECK(vfs_statvfs_path("/selftest/mnt", &sv) == 0 &&
            sv.f_blocks == 16 && strcmp(sv.f_basetype, "tmpfs") == 0);
        {
                struct vnode *fv;

                CHECK(vn_open("/selftest/mnt/big", FWRITE | FCREAT, 0644,
                    &fv) == 0);
                if (fv != NULL) {
                        CHECK(vn_rdwr(UIO_WRITE, fv, big, 128 * 1024, 0, 0,
                            &resid) == 0);
                        CHECK(resid == 128 * 1024 - 15 * PAGESIZE);
                        CHECK(vn_rdwr(UIO_WRITE, fv, big, 10, 1 << 20, 0,
                            &resid) == ENOSPC);
                        (void)vn_close(fv, FWRITE);
                }
        }

        CHECK(vfs_unmount("/selftest/mnt") == EBUSY);   /* vp is held */
        if (vp != NULL)
                VN_RELE(vp);
        CHECK(vfs_unmount("/selftest/mnt") == 0);
        CHECK(get("/selftest/mnt/hidden", buf, 8, 0) == 1);
        CHECK(get("/selftest/mnt/inside", buf, 8, 0) == -ENOENT);
        CHECK(vfs_unmount("/selftest/mnt") == EINVAL);  /* not a root */
        CHECK(vfs_unmount("/") == EBUSY);
        CHECK(vn_remove("/selftest/mnt/hidden") == 0);
        CHECK(vn_rmdir("/selftest/mnt") == 0);
        kmem_free(big, 128 * 1024);
}

static void
test_dnlc(void)
{
        struct dnlc_stats before, after;
        struct vnode *vp;

        CHECK(put("/selftest/cached", "c", 1, 0) == 0);
        vp = look("/selftest/cached", FOLLOW);
        if (vp != NULL)
                VN_RELE(vp);
        dnlc_get_stats(&before);
        vp = look("/selftest/cached", FOLLOW);
        if (vp != NULL)
                VN_RELE(vp);
        dnlc_get_stats(&after);
        CHECK(after.ds_hits >= before.ds_hits + 2);     /* both components */
        CHECK(vn_remove("/selftest/cached") == 0);
        CHECK(lookupname("/selftest/cached", FOLLOW, &vp) == ENOENT);
}

/* Threads creating, writing, checking, and removing files at once. */
#define VFS_THREADS     8
#define VFS_FILES       40

static struct sema vfs_done;
static volatile uint32_t vfs_errors;

static void
vfs_worker(void *arg)
{
        unsigned int me = (unsigned int)(uintptr_t)arg;
        char path[64], data[64], back[64];

        for (int i = 0; i < VFS_FILES; i++) {
                int len = snprintf(data, sizeof(data), "thread %u file %d",
                    me, i);

                /* Half in a shared directory, half in our own. */
                if ((i & 1) == 0)
                        snprintf(path, sizeof(path),
                            "/selftest/shared/t%u-%d", me, i);
                else
                        snprintf(path, sizeof(path), "/selftest/own%u/%d", me,
                            i);
                if (put(path, data, (size_t)len, 0) != 0 ||
                    get(path, back, sizeof(back), 0) != len ||
                    memcmp(back, data, (size_t)len) != 0)
                        atomic_inc_32_nv(&vfs_errors);
        }
        for (int i = 0; i < VFS_FILES; i++) {
                if ((i & 1) == 0)
                        snprintf(path, sizeof(path),
                            "/selftest/shared/t%u-%d", me, i);
                else
                        snprintf(path, sizeof(path), "/selftest/own%u/%d", me,
                            i);
                if (vn_remove(path) != 0)
                        atomic_inc_32_nv(&vfs_errors);
        }
        vsema(&vfs_done);
}

static void
test_concurrency(void)
{
        char path[64];

        CHECK(vn_mkdir("/selftest/shared", 0755) == 0);
        for (unsigned int t = 0; t < VFS_THREADS; t++) {
                snprintf(path, sizeof(path), "/selftest/own%u", t);
                CHECK(vn_mkdir(path, 0755) == 0);
        }
        sema_init(&vfs_done, 0, "vfs_done");
        vfs_errors = 0;
        for (uintptr_t t = 0; t < VFS_THREADS; t++)
                (void)kthread_create(vfs_worker, (void *)t, PRI_KERNEL,
                    "vfsw");
        for (int t = 0; t < VFS_THREADS; t++)
                psema(&vfs_done);
        CHECK(vfs_errors == 0);
        CHECK(vn_rmdir("/selftest/shared") == 0);       /* empty again */
        for (unsigned int t = 0; t < VFS_THREADS; t++) {
                snprintf(path, sizeof(path), "/selftest/own%u", t);
                CHECK(vn_rmdir(path) == 0);
        }
}

int
selftest_vfs(void)
{
        struct tmount *tm = rootvfs->vfs_data;
        uint64_t pages0 = tm->tm_pages;
        uint32_t nodes0 = tm->tm_nnodes;
        struct dnlc_stats ds;

        failures = 0;
        test_lookup_rules();
        test_file_data();
        test_links_and_rename();
        test_symlinks();
        test_readdir();
        test_mounts();
        test_dnlc();
        test_concurrency();

        /* Tear down what test_lookup_rules made. */
        CHECK(vn_remove("/selftest/a/b/f") == 0);
        CHECK(vn_rmdir("/selftest/a/b") == 0);
        CHECK(vn_rmdir("/selftest/a") == 0);
        CHECK(vn_rmdir("/selftest") == 0);

        /* Everything must be back, cached names or not. */
        dnlc_purge_vfsp(rootvfs);
        CHECK(tm->tm_pages == pages0);
        CHECK(tm->tm_nnodes == nodes0);
        dnlc_get_stats(&ds);
        printf("selftest: vfs: %u tmpfs nodes, %ju pages; dnlc %ju hits, "
            "%ju misses\n", tm->tm_nnodes, (uintmax_t)tm->tm_pages,
            (uintmax_t)ds.ds_hits, (uintmax_t)ds.ds_misses);
        return failures;
}
