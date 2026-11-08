/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for the buffer cache and ext2fs, on the images that
 * tools/mkext2.sh builds: vd1 with 1 KB blocks and vd2 with 4 KB
 * blocks. Each is mounted, read, changed, unmounted, remounted, and
 * checked again; the files left in /persist.txt and /pdir are read back
 * on the host with debugfs, and both images must pass e2fsck. An entry
 * is also left in /many, which mke2fs built with a hash index, so fsck
 * checks that changing an indexed directory dropped the index.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/buf.h>
#include <sys/cred.h>
#include <sys/dirent.h>
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

#include "fs/ext2fs/ext2fs.h"

int selftest_ext2(void);

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

static char *
P(char *buf, const char *mnt, const char *rel)
{
        snprintf(buf, MAXPATHLEN, "%s%s", mnt, rel);
        return buf;
}

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

static int
same(const char *path, const char *want)
{
        char buf[256];
        long n = get(path, buf, sizeof(buf), 0);

        return n == (long)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

static unsigned char
big_byte(size_t i)
{
        return (unsigned char)((i * 7 + 3) % 95 + 32);
}

/* Count directory entries other than "." and "..". */
static int
count_entries(const char *path)
{
        char *buf = kmem_alloc(2048, KM_SLEEP);
        struct vnode *vp;
        struct iovec iov;
        struct uio uio;
        int eof = 0, n = 0, guard = 0;

        if (lookupname(path, FOLLOW, &vp) != 0) {
                kmem_free(buf, 2048);
                return -1;
        }
        uio.uio_offset = 0;
        while (!eof && guard++ < 1000) {
                size_t used, pos = 0;

                iov.iov_base = buf;
                iov.iov_len = 2048;
                uio.uio_iov = &iov;
                uio.uio_iovcnt = 1;
                uio.uio_resid = 2048;
                uio.uio_segflg = UIO_SYSSPACE;
                uio.uio_rw = UIO_READ;
                if (VOP_READDIR(vp, &uio, &eof, kcred) != 0) {
                        n = -1;
                        break;
                }
                used = 2048 - (size_t)uio.uio_resid;
                while (pos < used) {
                        struct dirent *d = (struct dirent *)(void *)(buf + pos);

                        if (strcmp(d->d_name, ".") != 0 &&
                            strcmp(d->d_name, "..") != 0)
                                n++;
                        pos += d->d_reclen;
                }
        }
        VN_RELE(vp);
        kmem_free(buf, 2048);
        return n;
}

static void
statfs(const char *mnt, uint64_t *bfree, uint64_t *ffree)
{
        struct statvfs sv;

        *bfree = *ffree = 0;
        if (vfs_statvfs_path(mnt, &sv) == 0) {
                *bfree = sv.f_bfree;
                *ffree = sv.f_ffree;
        }
}

/* What mke2fs put there. */
static void
test_existing(const char *mnt)
{
        unsigned char *buf = kmem_alloc(300000, KM_SLEEP);
        char p[MAXPATHLEN], text[MAXPATHLEN];
        struct vattr va;
        size_t len;
        int ok = 1, all = 1;
        long n;

        CHECK(same(P(p, mnt, "/hello.txt"), "Hello from mke2fs\n"));
        CHECK(same(P(p, mnt, "/dir/sub/deep.txt"), "deep\n"));
        CHECK(same(P(p, mnt, "/link"), "Hello from mke2fs\n"));
        CHECK(vn_readlink(P(p, mnt, "/link"), text, sizeof(text), &len) ==
            0 && strcmp(text, "hello.txt") == 0);
        CHECK(vn_readlink(P(p, mnt, "/longlink"), text, sizeof(text), &len) ==
            0 && len == 100 && text[0] == 'x' && text[99] == 'x');

        CHECK(vn_stat(P(p, mnt, "/big.bin"), FOLLOW, &va) == 0 &&
            va.va_size == 300000 && va.va_type == VREG);
        /* Unaligned chunks across direct, indirect, double indirect. */
        for (size_t off = 0; off < 300000; off += 7000) {
                size_t want = 300000 - off < 7000 ? 300000 - off : 7000;

                n = get(P(p, mnt, "/big.bin"), buf, want, (off_t)off);
                if (n != (long)want) {
                        ok = 0;
                        break;
                }
                for (size_t i = 0; i < want; i++)
                        ok &= buf[i] == big_byte(off + i);
        }
        CHECK(ok);

        CHECK(count_entries(P(p, mnt, "/many")) == 200);
        for (int i = 0; i < 200; i++) {
                char rel[32], name[8];

                snprintf(name, sizeof(name), "f%03d", i);
                snprintf(rel, sizeof(rel), "/many/%s", name);
                all &= same(P(p, mnt, rel), name);
        }
        CHECK(all);
        CHECK(get(P(p, mnt, "/many/f200"), buf, 4, 0) == -ENOENT);
        kmem_free(buf, 300000);
}

static void
test_file_data(const char *mnt)
{
        unsigned char *buf = kmem_alloc(100000, KM_SLEEP);
        unsigned char *chk = kmem_alloc(100000, KM_SLEEP);
        uint64_t b0, f0, b1, f1;
        char p[MAXPATHLEN];
        struct vattr va;
        int ok = 1;

        statfs(mnt, &b0, &f0);
        for (size_t i = 0; i < 100000; i++)
                buf[i] = (unsigned char)(i * 13 + 5);
        CHECK(put(P(p, mnt, "/new.bin"), buf, 100000, 0) == 0);
        CHECK(get(p, chk, 100000, 0) == 100000 &&
            memcmp(buf, chk, 100000) == 0);

        /* Overwrite the middle, unaligned. */
        memset(buf + 50001, 0xee, 4999);
        CHECK(put(p, buf + 50001, 4999, 50001) == 0);
        CHECK(get(p, chk, 100000, 0) == 100000 &&
            memcmp(buf, chk, 100000) == 0);

        /* Truncate, extend; the gap reads zero. */
        CHECK(vn_truncate(p, 1000) == 0);
        CHECK(vn_truncate(p, 3000) == 0);
        CHECK(get(p, chk, 3000, 0) == 3000);
        ok = memcmp(chk, buf, 1000) == 0;
        for (int i = 1000; i < 3000; i++)
                ok &= chk[i] == 0;
        CHECK(ok);

        /* A sparse write far out; the hole reads zero. */
        CHECK(put(p, "tail", 4, 3 * 1024 * 1024) == 0);
        CHECK(vn_stat(p, FOLLOW, &va) == 0 &&
            va.va_size == 3 * 1024 * 1024 + 4);
        CHECK(get(p, chk, 512, 2 * 1024 * 1024) == 512);
        ok = 1;
        for (int i = 0; i < 512; i++)
                ok &= chk[i] == 0;
        CHECK(ok);

        CHECK(vn_remove(p) == 0);
        statfs(mnt, &b1, &f1);
        CHECK(b1 == b0 && f1 == f0);    /* every block and inode back */
        kmem_free(buf, 100000);
        kmem_free(chk, 100000);
}

static void
test_names(const char *mnt)
{
        char p[MAXPATHLEN], q[MAXPATHLEN];
        uint64_t b0, f0, b1, f1;
        struct vattr va;
        int all = 1;

        statfs(mnt, &b0, &f0);
        CHECK(vn_mkdir(P(p, mnt, "/t"), 0755) == 0);
        CHECK(vn_mkdir(P(p, mnt, "/t/a"), 0755) == 0);
        CHECK(vn_mkdir(P(p, mnt, "/t/a/b"), 0755) == 0);
        CHECK(put(P(p, mnt, "/t/a/b/f"), "f", 1, 0) == 0);
        CHECK(vn_stat(P(p, mnt, "/t"), FOLLOW, &va) == 0 && va.va_nlink == 3);
        CHECK(vn_rmdir(P(p, mnt, "/t/a")) == ENOTEMPTY);
        CHECK(vn_rename(P(p, mnt, "/t"), P(q, mnt, "/t/a/b/x")) == EINVAL);
        CHECK(vn_rename(P(p, mnt, "/t/a"), P(q, mnt, "/t2")) == 0);
        CHECK(same(P(p, mnt, "/t2/b/f"), "f"));
        {
                struct vnode *a, *b;

                CHECK(lookupname(P(p, mnt, "/t2/.."), FOLLOW, &a) == 0);
                CHECK(lookupname(mnt, FOLLOW, &b) == 0);
                CHECK(a == b);
                VN_RELE(a);
                VN_RELE(b);
        }
        CHECK(vn_stat(P(p, mnt, "/t"), FOLLOW, &va) == 0 && va.va_nlink == 2);

        CHECK(vn_link(P(p, mnt, "/t2/b/f"), P(q, mnt, "/t/hard")) == 0);
        CHECK(vn_stat(P(p, mnt, "/t/hard"), FOLLOW, &va) == 0 &&
            va.va_nlink == 2);
        CHECK(put(P(p, mnt, "/t/other"), "o", 1, 0) == 0);
        CHECK(vn_rename(P(p, mnt, "/t/other"), P(q, mnt, "/t/hard")) == 0);
        CHECK(same(P(p, mnt, "/t/hard"), "o"));
        CHECK(vn_stat(P(p, mnt, "/t2/b/f"), FOLLOW, &va) == 0 &&
            va.va_nlink == 1);
        CHECK(vn_symlink("../t2/b/f", P(p, mnt, "/t/sym")) == 0);
        CHECK(same(P(p, mnt, "/t/sym"), "f"));

        /* A directory spanning many blocks, with holes punched in it. */
        CHECK(vn_mkdir(P(p, mnt, "/t/bulk"), 0755) == 0);
        for (int i = 0; i < 300; i++) {
                char rel[64];

                snprintf(rel, sizeof(rel), "/t/bulk/name-%04d", i);
                all &= put(P(p, mnt, rel), rel, strlen(rel), 0) == 0;
        }
        CHECK(all);
        CHECK(count_entries(P(p, mnt, "/t/bulk")) == 300);
        for (int i = 0; i < 300; i += 2) {
                char rel[64];

                snprintf(rel, sizeof(rel), "/t/bulk/name-%04d", i);
                all &= vn_remove(P(p, mnt, rel)) == 0;
        }
        CHECK(all);
        CHECK(count_entries(P(p, mnt, "/t/bulk")) == 150);
        CHECK(get(P(p, mnt, "/t/bulk/name-0000"), q, 4, 0) == -ENOENT);
        CHECK(same(P(p, mnt, "/t/bulk/name-0001"), "/t/bulk/name-0001"));
        for (int i = 1; i < 300; i += 2) {
                char rel[64];

                snprintf(rel, sizeof(rel), "/t/bulk/name-%04d", i);
                all &= vn_remove(P(p, mnt, rel)) == 0;
        }
        CHECK(all);
        CHECK(vn_rmdir(P(p, mnt, "/t/bulk")) == 0);

        /* An indexed directory from mke2fs takes a new entry and loses one. */
        CHECK(put(P(p, mnt, "/many/added"), "a", 1, 0) == 0);
        CHECK(count_entries(P(p, mnt, "/many")) == 201);
        CHECK(vn_remove(P(p, mnt, "/many/added")) == 0);

        CHECK(vn_remove(P(p, mnt, "/t/sym")) == 0);
        CHECK(vn_remove(P(p, mnt, "/t/hard")) == 0);
        CHECK(vn_remove(P(p, mnt, "/t2/b/f")) == 0);
        CHECK(vn_rmdir(P(p, mnt, "/t2/b")) == 0);
        CHECK(vn_rmdir(P(p, mnt, "/t2")) == 0);
        CHECK(vn_rmdir(P(p, mnt, "/t")) == 0);
        statfs(mnt, &b1, &f1);
        /* The indexed directory may have grown by a block; nothing else. */
        CHECK(f1 == f0 && b1 + 1 >= b0 && b1 <= b0);
}

static void
test_full(const char *mnt)
{
        unsigned char *chunk = kmem_alloc(65536, KM_SLEEP);
        uint64_t b0, f0, b1, f1;
        char p[MAXPATHLEN];
        struct vnode *vp;
        size_t resid = 0;
        off_t off = 0;
        int error = 0;

        statfs(mnt, &b0, &f0);
        memset(chunk, 0x5a, 65536);
        CHECK(vn_open(P(p, mnt, "/fill"), FWRITE | FCREAT, 0644, &vp) == 0);
        while (vp != NULL && error == 0 && resid == 0) {
                error = vn_rdwr(UIO_WRITE, vp, chunk, 65536, off, 0, &resid);
                off += (off_t)(65536 - resid);
        }
        if (vp != NULL) {
                CHECK(resid != 0 || error == ENOSPC);
                CHECK(vn_rdwr(UIO_WRITE, vp, chunk, 1024, off, 0, &resid) ==
                    ENOSPC);
                (void)vn_close(vp, FWRITE);
        }
        statfs(mnt, &b1, &f1);
        CHECK(b1 == 0);
        CHECK(vn_remove(p) == 0);
        statfs(mnt, &b1, &f1);
        CHECK(b1 == b0 && f1 == f0);
        printf("selftest: %s: filled with %jd KB, freed again\n", mnt,
            (intmax_t)(off / 1024));
        kmem_free(chunk, 65536);
}

#define E2_THREADS      8
#define E2_FILES        20

static struct sema e2_done;
static volatile uint32_t e2_errors;
static const char *e2_mnt;

static void
e2_worker(void *arg)
{
        unsigned int me = (unsigned int)(uintptr_t)arg;
        char p[MAXPATHLEN], rel[64], data[64];

        for (int i = 0; i < E2_FILES; i++) {
                int len = snprintf(data, sizeof(data), "t%u f%d", me, i);

                snprintf(rel, sizeof(rel), "/conc/%u-%d", me, i);
                if (put(P(p, e2_mnt, rel), data, (size_t)len, 0) != 0 ||
                    !same(p, data))
                        atomic_inc_32_nv(&e2_errors);
        }
        for (int i = 0; i < E2_FILES; i++) {
                snprintf(rel, sizeof(rel), "/conc/%u-%d", me, i);
                if (vn_remove(P(p, e2_mnt, rel)) != 0)
                        atomic_inc_32_nv(&e2_errors);
        }
        vsema(&e2_done);
}

static void
test_concurrency(const char *mnt)
{
        char p[MAXPATHLEN];

        CHECK(vn_mkdir(P(p, mnt, "/conc"), 0755) == 0);
        sema_init(&e2_done, 0, "e2_done");
        e2_errors = 0;
        e2_mnt = mnt;
        for (uintptr_t t = 0; t < E2_THREADS; t++)
                (void)kthread_create(e2_worker, (void *)t, PRI_KERNEL, "e2w");
        for (int t = 0; t < E2_THREADS; t++)
                psema(&e2_done);
        CHECK(e2_errors == 0);
        CHECK(count_entries(p) == 0);
        CHECK(vn_rmdir(p) == 0);
}

static void
test_filesystem(const char *dev, const char *mnt, int fill)
{
        struct ext2fs_args args = { dev, 0 };
        struct ext2fs_args roargs = { dev, 1 };
        char p[MAXPATHLEN];

        CHECK(vn_mkdir(mnt, 0755) == 0);
        CHECK(vfs_mount("ext2fs", mnt, &args) == 0);
        test_existing(mnt);
        test_file_data(mnt);
        test_names(mnt);
        if (fill)
                test_full(mnt);
        test_concurrency(mnt);

        /* Left for the host to read back. */
        CHECK(put(P(p, mnt, "/persist.txt"), "written by belka", 16, 0) == 0);
        CHECK(vn_mkdir(P(p, mnt, "/pdir"), 0755) == 0);
        CHECK(vn_mkdir(P(p, mnt, "/pdir/sub"), 0755) == 0);
        CHECK(put(P(p, mnt, "/pdir/sub/inner.txt"), "inner", 5, 0) == 0);
        /* An entry the hash index from mke2fs does not know about. */
        CHECK(put(P(p, mnt, "/many/zz-belka"), "kept", 4, 0) == 0);

        /* Through the disk: unmount, mount again, read. */
        CHECK(vfs_unmount(mnt) == 0);
        CHECK(vfs_mount("ext2fs", mnt, &args) == 0);
        CHECK(same(P(p, mnt, "/persist.txt"), "written by belka"));
        CHECK(same(P(p, mnt, "/pdir/sub/inner.txt"), "inner"));
        CHECK(same(P(p, mnt, "/hello.txt"), "Hello from mke2fs\n"));
        CHECK(vfs_unmount(mnt) == 0);

        /* Read-only. */
        CHECK(vfs_mount("ext2fs", mnt, &roargs) == 0);
        CHECK(same(P(p, mnt, "/persist.txt"), "written by belka"));
        CHECK(put(P(p, mnt, "/nope"), "x", 1, 0) == EROFS);
        CHECK(vfs_unmount(mnt) == 0);
        CHECK(vn_rmdir(mnt) == 0);
}

int
selftest_ext2(void)
{
        struct bc_stats bs;

        failures = 0;
        if (blkdev_lookup("vd1") == NULL || blkdev_lookup("vd2") == NULL) {
                printf("selftest: ext2: test disks vd1 and vd2 missing\n");
                return 1;
        }
        CHECK(vfs_mount("ext2fs", "/", NULL) != 0);     /* no device */
        test_filesystem("vd1", "/e1", 1);
        test_filesystem("vd2", "/e4", 0);
        bc_get_stats(&bs);
        printf("selftest: ext2: buffer cache %u buffers, %ju hits, %ju "
            "misses, %ju reads, %ju writes\n", bs.bs_nbuf,
            (uintmax_t)bs.bs_hits, (uintmax_t)bs.bs_misses,
            (uintmax_t)bs.bs_reads, (uintmax_t)bs.bs_writes);
        CHECK(bs.bs_ndirty == 0);
        return failures;
}
