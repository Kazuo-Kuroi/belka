/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for PCI and the virtio block driver, against the test disk
 * that tools/mkdisk.c writes: sector n begins with n (64-bit little
 * endian) and "BELKADSK", followed by (n * 7 + offset) mod 256. Writes
 * go only to the scratch area at the end of the disk.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
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

static struct blkdev *vd0;

/* A buffer whose pages are not physically adjacent. */
static void
test_scattered_buffer(void)
{
        struct page *pp[3];
        vaddr_t va = kva_alloc(3, KM_SLEEP);
        unsigned char *buf = (unsigned char *)(uintptr_t)va + 100;
        struct page *spacer = page_alloc(0, 0);

        for (int i = 0; i < 3; i++) {
                /* Allocate in reverse, with a page between, to scatter. */
                pp[2 - i] = page_alloc(0, PA_ZERO);
                (void)hat_kenter(va + (vaddr_t)(2 - i) * PAGESIZE,
                    page_to_pa(pp[2 - i]), HAT_WRITE);
        }
        CHECK(page_to_pa(pp[1]) != page_to_pa(pp[0]) + PAGESIZE);
        CHECK(blkdev_read(vd0, 1234, buf, 8) == 0);
        CHECK(pattern_ok(buf, 1234, 8));
        hat_kremove(va, 3);
        kva_free(va, 3);
        for (int i = 0; i < 3; i++)
                page_free(pp[i], 0);
        page_free(spacer, 0);
}

static void
test_block_io(void)
{
        unsigned char *big = kmem_alloc(200 * SECSIZE, KM_SLEEP);
        unsigned char *w = kmem_alloc(16 * SECSIZE, KM_SLEEP);
        unsigned char one[SECSIZE];
        uint64_t seed = lbolt;

        vd0 = blkdev_lookup("vd0");
        CHECK(vd0 != NULL);
        if (vd0 == NULL)
                goto out;
        CHECK(vd0->bd_secsize == SECSIZE && vd0->bd_nsectors == DISK_SECTORS);
        CHECK(blkdev_lookup("vd9") == NULL);

        for (uint64_t s = 0; s < 32; s++) {
                CHECK(blkdev_read(vd0, s, one, 1) == 0);
                CHECK(pattern_ok(one, s, 1));
        }
        CHECK(blkdev_read(vd0, 4000, big, 128) == 0);   /* one 64 KB request */
        CHECK(pattern_ok(big, 4000, 128));
        CHECK(blkdev_read(vd0, 8100, big, 200) == 0);   /* split in two */
        CHECK(pattern_ok(big, 8100, 200));
        test_scattered_buffer();

        /* Out of range. */
        CHECK(blkdev_read(vd0, DISK_SECTORS, one, 1) == ENXIO);
        CHECK(blkdev_read(vd0, DISK_SECTORS - 2, big, 4) == ENXIO);

        /* Write, read back, flush. */
        for (int i = 0; i < 16 * SECSIZE; i++)
                w[i] = (unsigned char)((uint64_t)i * 13 + seed);
        CHECK(blkdev_write(vd0, SCRATCH_FIRST + 100, w, 16) == 0);
        memset(big, 0, 16 * SECSIZE);
        CHECK(blkdev_read(vd0, SCRATCH_FIRST + 100, big, 16) == 0);
        CHECK(memcmp(big, w, 16 * SECSIZE) == 0);
        CHECK(blkdev_flush(vd0) == 0);
out:
        kmem_free(big, 200 * SECSIZE);
        kmem_free(w, 16 * SECSIZE);
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
                if (blkdev_read(vd0, sector, buf, n) != 0 ||
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

        if (vd0 == NULL)
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
        printf("selftest: vd0: %u concurrent reads in %ju ticks, %u bad\n",
            reads_done, (uintmax_t)(lbolt - start), read_errors);
}

int
selftest_dev(void)
{
        failures = 0;
        test_pci();
        test_block_io();
        test_concurrent_reads();
        return failures;
}
