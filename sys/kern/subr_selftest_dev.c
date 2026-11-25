/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for PCI and the block drivers (virtio, NVMe, AHCI), against
 * copies of the test disk that tools/mkdisk.c writes: sector n begins
 * with n (64-bit little endian) and "BELKADSK", followed by
 * (n * 7 + offset) mod 256. Writes go only to the scratch area at the
 * end of the disk.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/random.h>
#include <sys/siphash.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>

#include "dev/pci/pcivar.h"

#define DISK_SECTORS    16384
#define SCRATCH_FIRST   16000           /* tests may write from here */
#define SECSIZE         512

int selftest_dev(void);

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

/* Nonzero if buf holds nsec sectors of the pattern from sector first. */
static int
pattern_ok(const unsigned char *buf, uint64_t first, size_t nsec)
{
        for (size_t s = 0; s < nsec; s++) {
                const unsigned char *p = buf + s * SECSIZE;
                uint64_t n = first + s, got = 0;

                for (int i = 0; i < 8; i++)
                        got |= (uint64_t)p[i] << (8 * i);
                if (got != n || memcmp(p + 8, "BELKADSK", 8) != 0)
                        return 0;
                for (int i = 16; i < SECSIZE; i++) {
                        if (p[i] != (unsigned char)((n * 7 + (uint64_t)i) &
                            0xff))
                                return 0;
                }
        }
        return 1;
}

static void
test_pci(void)
{
        struct pci_dev *pd = pci_device(0);
        /* A modern-only (0x1042) or transitional (0x1001) block device. */
        struct pci_dev *vd = pci_find(0x1af4, 0x1042, 0);
        int membar = -1;

        CHECK(pci_ndevices() >= 2);
        CHECK(pd != NULL && pd->pd_class == PCI_CLASS_BRIDGE &&
            pd->pd_subclass == PCI_SUBCLASS_BRIDGE_HOST);
        if (vd == NULL)
                vd = pci_find(0x1af4, 0x1001, 0);
        CHECK(vd != NULL);
        if (vd == NULL)
                return;
        CHECK(vd->pd_driver != NULL);
        for (int i = 0; i < PCI_NBARS; i++) {
                if (vd->pd_bar[i].pb_addr != 0 && !vd->pd_bar[i].pb_io &&
                    vd->pd_bar[i].pb_size >= 0x4000)
                        membar = i;
        }
        CHECK(membar >= 0);
        CHECK(pci_msix_count(vd) >= 2);
        CHECK(pci_find_cap(vd, PCI_CAP_VENDOR, 0) != 0);
        CHECK(pci_find_cap(vd, 0x7f, 0) == 0);
}

static struct blkdev *disk;     /* the one under test */

/* A buffer whose pages are not physically adjacent. */
static void
test_scattered_buffer(void)
{
        struct page *pp[3];
        vaddr_t va = kva_alloc(3, KM_SLEEP);
        unsigned char *buf = (unsigned char *)(uintptr_t)va + 100;

        for (int i = 0; i < 3; i++)
                pp[i] = page_alloc(0, PA_ZERO);
        /*
         * Whatever order the allocator gave them in, map them so that the
         * second page does not follow the first in physical memory.
         */
        if (page_to_pa(pp[1]) == page_to_pa(pp[0]) + PAGESIZE) {
                struct page *t = pp[1];

                pp[1] = pp[2];
                pp[2] = t;
        }
        for (int i = 0; i < 3; i++)
                (void)hat_kenter(va + (vaddr_t)i * PAGESIZE,
                    page_to_pa(pp[i]), HAT_WRITE);
        CHECK(page_to_pa(pp[1]) != page_to_pa(pp[0]) + PAGESIZE);
        CHECK(blkdev_read(disk, 1234, buf, 8) == 0);
        CHECK(pattern_ok(buf, 1234, 8));
        hat_kremove(va, 3);
        kva_free(va, 3);
        for (int i = 0; i < 3; i++)
                page_free(pp[i], 0);
}

static void
test_block_io(const char *name)
{
        unsigned char *big = kmem_alloc(200 * SECSIZE, KM_SLEEP);
        unsigned char *w = kmem_alloc(16 * SECSIZE, KM_SLEEP);
        unsigned char one[SECSIZE];
        uint64_t seed = lbolt;

        disk = blkdev_lookup(name);
        CHECK(disk != NULL);
        if (disk == NULL) {
                printf("selftest: no disk %s\n", name);
                goto out;
        }
        CHECK(disk->bd_secsize == SECSIZE &&
            disk->bd_nsectors == DISK_SECTORS);
        CHECK(blkdev_lookup("vd9") == NULL);

        for (uint64_t s = 0; s < 32; s++) {
                CHECK(blkdev_read(disk, s, one, 1) == 0);
                CHECK(pattern_ok(one, s, 1));
        }
        CHECK(blkdev_read(disk, 4000, big, 128) == 0);   /* one 64 KB request */
        CHECK(pattern_ok(big, 4000, 128));
        CHECK(blkdev_read(disk, 8100, big, 200) == 0);   /* split in two */
        CHECK(pattern_ok(big, 8100, 200));
        test_scattered_buffer();

        /* Out of range. */
        CHECK(blkdev_read(disk, DISK_SECTORS, one, 1) == ENXIO);
        CHECK(blkdev_read(disk, DISK_SECTORS - 2, big, 4) == ENXIO);

        /* Write, read back, flush. */
        for (int i = 0; i < 16 * SECSIZE; i++)
                w[i] = (unsigned char)((uint64_t)i * 13 + seed);
        CHECK(blkdev_write(disk, SCRATCH_FIRST + 100, w, 16) == 0);
        memset(big, 0, 16 * SECSIZE);
        CHECK(blkdev_read(disk, SCRATCH_FIRST + 100, big, 16) == 0);
        CHECK(memcmp(big, w, 16 * SECSIZE) == 0);
        CHECK(blkdev_flush(disk) == 0);
out:
        kmem_free(big, 200 * SECSIZE);
        kmem_free(w, 16 * SECSIZE);
}


/*
 * GPT partitions (kern/subr_gpt.c), on vd3, the test disk with
 * a table written in by tools/mkgpt.c (Makefile, GPT_PARTS): entries 1,
 * 2 and 4, so vd3p1, vd3p2 and vd3p4 and no vd3p3; each partition's
 * sectors are the disk's from its start, in range only; a write to a
 * partition lands in the disk's sector. With the primary header, or
 * then the primary entries, broken, the table is read from the backup;
 * mended, from the primary again.
 */
static void
test_partitions(void)
{
        struct blkdev *d = blkdev_lookup("vd3"), *p1 = blkdev_lookup("vd3p1"),
            *p2 = blkdev_lookup("vd3p2"), *p4 = blkdev_lookup("vd3p4");
        unsigned char *buf = kmem_alloc(4 * SECSIZE, KM_SLEEP);
        unsigned char *w = kmem_alloc(SECSIZE, KM_SLEEP);
        unsigned char *save = kmem_alloc(2 * SECSIZE, KM_SLEEP);
        int backup = -1;

        CHECK(d != NULL && p1 != NULL && p2 != NULL && p4 != NULL);
        CHECK(blkdev_lookup("vd3p3") == NULL);
        if (d == NULL || p1 == NULL || p2 == NULL || p4 == NULL)
                goto out;
        CHECK(p1->bd_parent == d && p1->bd_start == 2048 &&
            p1->bd_nsectors == 2048 && p1->bd_secsize == SECSIZE);
        CHECK(p2->bd_start == 4096 && p2->bd_nsectors == 8192);
        CHECK(p4->bd_start == 12288 && p4->bd_nsectors == 16000 - 12288 + 1);

        CHECK(blkdev_read(p1, 0, buf, 4) == 0 && pattern_ok(buf, 2048, 4));
        CHECK(blkdev_read(p2, 100, buf, 1) == 0 && pattern_ok(buf, 4196, 1));
        CHECK(blkdev_read(p2, 8190, buf, 2) == 0 &&
            pattern_ok(buf, 4096 + 8190, 2));
        CHECK(blkdev_read(p2, 8191, buf, 2) == ENXIO);
        CHECK(blkdev_read(p1, 2048, buf, 1) == ENXIO);

        /* A write to the partition, read back through the disk. */
        for (int i = 0; i < SECSIZE; i++)
                w[i] = (unsigned char)(i * 5 + 1);
        CHECK(blkdev_write(p4, 3000, w, 1) == 0);
        CHECK(blkdev_read(d, 12288 + 3000, buf, 1) == 0 &&
            memcmp(buf, w, SECSIZE) == 0);
        CHECK(blkdev_flush(p4) == 0);

        /* The backup table, when the primary header or entries are bad. */
        CHECK(blkdev_gpt_probe(d, &backup) == 3 && backup == 0);
        CHECK(blkdev_read(d, 1, save, 2) == 0);
        memset(buf, 0, SECSIZE);
        CHECK(blkdev_write(d, 1, buf, 1) == 0);
        CHECK(blkdev_gpt_probe(d, &backup) == 3 && backup == 1);
        CHECK(blkdev_write(d, 1, save, 1) == 0);
        memcpy(buf, save + SECSIZE, SECSIZE);
        buf[0] ^= 0xff;                 /* the first entry's type */
        CHECK(blkdev_write(d, 2, buf, 1) == 0);
        CHECK(blkdev_gpt_probe(d, &backup) == 3 && backup == 1);
        CHECK(blkdev_write(d, 2, save + SECSIZE, 1) == 0);
        CHECK(blkdev_gpt_probe(d, &backup) == 3 && backup == 0);
        CHECK(blkdev_gpt_probe(blkdev_lookup("vd0"), &backup) == -1);
out:
        kmem_free(buf, 4 * SECSIZE);
        kmem_free(w, SECSIZE);
        kmem_free(save, 2 * SECSIZE);
}

/* CRC-32 (libkern/crc32.c): the check value, and in two pieces. */
static void
test_crc32(void)
{
        static const char nine[] = "123456789";

        CHECK(crc32(0, nine, 9) == 0xcbf43926u);
        CHECK(crc32(crc32(0, nine, 4), nine + 4, 5) == 0xcbf43926u);
        CHECK(crc32(0, nine, 0) == 0);
}


/*
 * Recovery from a lost completion (blkdev_lose), on the NVMe and AHCI
 * disks, with the timeout cut to a second: the read times out and the
 * driver resets the controller or the port; the next read works.
 */
static void
test_recovery(const char *name)
{
        struct blkdev *bd = blkdev_lookup(name);
        unsigned char *buf = kmem_alloc(SECSIZE, KM_SLEEP);
        long saved = blkdev_timeout;
        uint64_t start = lbolt;

        CHECK(bd != NULL);
        if (bd == NULL)
                goto out;
        blkdev_timeout = HZ;
        atomic_store_32(&blkdev_lose, 1);
        CHECK(blkdev_read(bd, 10, buf, 1) == ETIMEDOUT);
        CHECK(atomic_load_32(&blkdev_lose) == 0);
        CHECK(lbolt - start >= HZ);
        CHECK(blkdev_read(bd, 11, buf, 1) == 0 && pattern_ok(buf, 11, 1));
        CHECK(blkdev_read(bd, 4000, buf, 1) == 0 && pattern_ok(buf, 4000, 1));
        blkdev_timeout = saved;
out:
        kmem_free(buf, SECSIZE);
}

/* Several threads reading at once, from different sectors. */
#define READERS         8
#define READS           100

static struct sema readers_done;
static volatile uint32_t read_errors, reads_done;

static void
reader_thread(void *arg)
{
        uint32_t seed = (uint32_t)(uintptr_t)arg * 2654435761u + 1u;
        unsigned char *buf = kmem_alloc(16 * SECSIZE, KM_SLEEP);

        for (int i = 0; i < READS; i++) {
                uint64_t sector;
                size_t n;

                seed = seed * 1103515245u + 12345u;
                n = 1 + (seed >> 8) % 16;
                seed = seed * 1103515245u + 12345u;
                sector = (seed >> 8) % (SCRATCH_FIRST - 16);
                if (blkdev_read(disk, sector, buf, n) != 0 ||
                    !pattern_ok(buf, sector, n))
                        atomic_inc_32_nv(&read_errors);
                atomic_inc_32_nv(&reads_done);
        }
        kmem_free(buf, 16 * SECSIZE);
        vsema(&readers_done);
}

static void
test_concurrent_reads(void)
{
        uint64_t start = lbolt;

        if (disk == NULL)
                return;
        sema_init(&readers_done, 0, "readers");
        read_errors = reads_done = 0;
        for (uintptr_t i = 0; i < READERS; i++)
                (void)kthread_create(reader_thread, (void *)i, PRI_KERNEL,
                    "reader");
        for (int i = 0; i < READERS; i++)
                psema(&readers_done);
        CHECK(reads_done == READERS * READS);
        CHECK(read_errors == 0);
        printf("selftest: %s: %u concurrent reads in %ju ticks, %u bad\n",
            disk->bd_name, reads_done, (uintmax_t)(lbolt - start), read_errors);
}

/* The random numbers: the cipher is right, and no two draws alike. */
static void
test_random(void)
{
        uint8_t a[64], b[64], z[64];

        memset(z, 0, sizeof(z));
        CHECK(rnd_selftest());
        rnd_get(a, sizeof(a));
        rnd_add("selftest", 8);
        rnd_get(b, sizeof(b));
        CHECK(memcmp(a, b, sizeof(a)) != 0 && memcmp(a, z, sizeof(a)) != 0 &&
            memcmp(b, z, sizeof(b)) != 0);
        rnd_get(a, sizeof(a));
        CHECK(memcmp(a, b, sizeof(a)) != 0);
}

/*
 * SipHash-2-4: the paper's vector (key 00..0f, message 00..0e), and the
 * empty message at each length below a word.
 */
static void
test_siphash(void)
{
        uint8_t key[SIPHASH_KEYLEN], msg[15];

        for (int i = 0; i < 16; i++)
                key[i] = (uint8_t)i;
        for (int i = 0; i < 15; i++)
                msg[i] = (uint8_t)i;
        CHECK(siphash24(key, msg, 15) == 0xa129ca6149be45e5ULL);
        CHECK(siphash24(key, msg, 0) == 0x726fdb47dd0e0e31ULL);
        CHECK(siphash24(key, msg, 8) == 0x93f5f5799a932462ULL);
}

int
selftest_dev(void)
{
        static const char *const disks[] = { "vd0", "nvd0", "sd0" };

        failures = 0;
        test_pci();
        test_random();
        test_siphash();
        test_crc32();
        /* The same disk image on each kind of disk (Makefile, QEMU_FLAGS). */
        for (size_t i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
                test_block_io(disks[i]);
                test_concurrent_reads();
        }
        test_partitions();
        test_recovery("nvd0");
        test_recovery("sd0");
        return failures;
}
