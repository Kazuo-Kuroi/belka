/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * GUID partition tables (the UEFI specification's GPT), read once the
 * drivers have attached (blkdev_scan_partitions, from configure()).
 * Each used entry of a disk's table becomes a block device of its own,
 * the disk's name, "p" and the entry's number from 1 (nvd0p2), whose
 * sectors are the partition's, range-checked, on the disk. Only GPT is
 * read: an MBR's own partitions, and BSD disklabels, are not.
 *
 * A table is taken if its header has the signature, sizes that make
 * sense and its own CRC-32 right, and its entry array its CRC-32: the
 * primary (header in sector 1), or else the backup (header in the
 * disk's last sector), each taken whole. Entries outside the usable
 * sectors, or ending before they start, are passed over. Partitions
 * are registered after every disk, so that the disks keep the devfs
 * minor numbers they had.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/systm.h>

#define GPT_SIGNATURE   "EFI PART"
#define GPT_HDR_MIN     92              /* bytes of the header defined */
#define GPT_MAXENTRIES  256
#define GPT_MAXARRAY    (128 * 1024)    /* bytes of entries read */
#define GPT_NAMELEN     36              /* UTF-16 code units */

/* The header, little-endian on disk; read field by field. */
#define GPT_H_SIG       0
#define GPT_H_HDRSIZE   12
#define GPT_H_HDRCRC    16
#define GPT_H_MYLBA     24
#define GPT_H_ALTLBA    32
#define GPT_H_FIRST     40
#define GPT_H_LAST      48
#define GPT_H_ENTLBA    72
#define GPT_H_NENT      80
#define GPT_H_ENTSIZE   84
#define GPT_H_ENTCRC    88

/* An entry. */
#define GPT_E_TYPE      0
#define GPT_E_START     32
#define GPT_E_END       40
#define GPT_E_NAME      56

struct gpt_part {
        struct blkdev   gp_bd;
};

static const struct {
        uint8_t         guid[16];       /* as on disk */
        const char      *name;
} gpt_types[] = {
        { { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b,
            0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }, "EFI system" },
        { { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e, 0x74, 0x4e,
            0x65, 0x65, 0x64, 0x45, 0x46, 0x49 }, "BIOS boot" },
        { { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79,
            0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 }, "Linux file system" },
        { { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43, 0x84, 0xe5,
            0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f }, "Linux swap" },
        { { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87, 0xc0,
            0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 }, "Microsoft basic data" },
};

static uint32_t
gpt_le32(const uint8_t *p)
{
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
            (uint32_t)p[3] << 24;
}

static uint64_t
gpt_le64(const uint8_t *p)
{
        return (uint64_t)gpt_le32(p) | (uint64_t)gpt_le32(p + 4) << 32;
}

/* A partition's I/O: its own sectors, on the disk. */
static int
gpt_read(struct blkdev *bd, uint64_t sector, void *buf, size_t nsec)
{
        return blkdev_read(bd->bd_parent, bd->bd_start + sector, buf, nsec);
}

static int
gpt_write(struct blkdev *bd, uint64_t sector, const void *buf, size_t nsec)
{
        return blkdev_write(bd->bd_parent, bd->bd_start + sector, buf, nsec);
}

static int
gpt_flush(struct blkdev *bd)
{
        return blkdev_flush(bd->bd_parent);
}

static const struct blkdev_ops gpt_ops = { gpt_read, gpt_write, gpt_flush };

/*
 * The header in sector lba, into hdr (a sector), if it is good: its
 * fields sane and its CRC right. 1 if so.
 */
static int
gpt_header(struct blkdev *disk, uint64_t lba, uint8_t *hdr)
{
        uint32_t size, crc, nent, entsize;
        uint64_t first, last;

        if (blkdev_read(disk, lba, hdr, 1) != 0 ||
            memcmp(hdr + GPT_H_SIG, GPT_SIGNATURE, 8) != 0)
                return 0;
        size = gpt_le32(hdr + GPT_H_HDRSIZE);
        if (size < GPT_HDR_MIN || size > disk->bd_secsize)
                return 0;
        crc = gpt_le32(hdr + GPT_H_HDRCRC);
        memset(hdr + GPT_H_HDRCRC, 0, 4);
        if (crc32(0, hdr, size) != crc)
                return 0;
        nent = gpt_le32(hdr + GPT_H_NENT);
        entsize = gpt_le32(hdr + GPT_H_ENTSIZE);
        first = gpt_le64(hdr + GPT_H_FIRST);
        last = gpt_le64(hdr + GPT_H_LAST);
        return gpt_le64(hdr + GPT_H_MYLBA) == lba && nent > 0 &&
            entsize >= 128 && (entsize & (entsize - 1)) == 0 &&
            (uint64_t)nent * entsize <= GPT_MAXARRAY &&
            first <= last && last < disk->bd_nsectors &&
            gpt_le64(hdr + GPT_H_ENTLBA) < disk->bd_nsectors;
}

/* An entry's type, by name, into out. */
static void
gpt_typename(const uint8_t *e, char *out, size_t len)
{
        const uint8_t *g = e + GPT_E_TYPE;

        for (size_t i = 0; i < sizeof(gpt_types) / sizeof(gpt_types[0]); i++)
                if (memcmp(g, gpt_types[i].guid, 16) == 0) {
                        snprintf(out, len, "%s", gpt_types[i].name);
                        return;
                }
        /* The GUID as it is written: its first three fields swapped. */
        snprintf(out, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0], g[5], g[4],
            g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* The entry's name, its ASCII characters (others as '?'), into out. */
static void
gpt_name(const uint8_t *e, char *out)
{
        int n;

        for (n = 0; n < GPT_NAMELEN; n++) {
                uint16_t c = (uint16_t)(e[GPT_E_NAME + 2 * n] |
                    e[GPT_E_NAME + 2 * n + 1] << 8);

                if (c == 0)
                        break;
                out[n] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        }
        out[n] = '\0';
}

/*
 * The table whose header is in sector lba: the header into hdr (a
 * sector), and its entry array, allocated, into *entsp (*entlenp
 * bytes). 1 if both are good; else 0, with nothing allocated.
 */
static int
gpt_load(struct blkdev *disk, uint64_t lba, uint8_t *hdr, uint8_t **entsp,
    size_t *entlenp)
{
        uint64_t entlba;
        size_t asize, nsec;
        uint8_t *ents;

        if (!gpt_header(disk, lba, hdr))
                return 0;
        entlba = gpt_le64(hdr + GPT_H_ENTLBA);
        asize = (size_t)gpt_le32(hdr + GPT_H_NENT) *
            gpt_le32(hdr + GPT_H_ENTSIZE);
        nsec = (asize + disk->bd_secsize - 1) / disk->bd_secsize;
        if (entlba + nsec > disk->bd_nsectors)
                return 0;
        ents = kmem_alloc(nsec * disk->bd_secsize, KM_SLEEP);
        if (blkdev_read(disk, entlba, ents, nsec) != 0 ||
            crc32(0, ents, asize) != gpt_le32(hdr + GPT_H_ENTCRC)) {
                kmem_free(ents, nsec * disk->bd_secsize);
                return 0;
        }
        *entsp = ents;
        *entlenp = nsec * disk->bd_secsize;
        return 1;
}

/*
 * Read disk's table, if it has one, and register its partitions if reg.
 * The number of partitions found, or -1 if there is no good table; in
 * *backup (if not NULL) whether the backup table was the one read.
 */
static int
gpt_scan(struct blkdev *disk, int reg, int *backupp)
{
        uint8_t *hdr = kmem_alloc(disk->bd_secsize, KM_SLEEP), *ents = NULL;
        uint32_t nent, entsize;
        uint64_t first, last;
        size_t entlen = 0;
        int backup = 0, nparts = -1;

        /* The primary, header and entries; else the backup, whole. */
        if (disk->bd_nsectors < 3)
                goto out;
        if (!gpt_load(disk, 1, hdr, &ents, &entlen)) {
                if (!gpt_load(disk, disk->bd_nsectors - 1, hdr, &ents,
                    &entlen))
                        goto out;
                backup = 1;
        }
        nent = gpt_le32(hdr + GPT_H_NENT);
        entsize = gpt_le32(hdr + GPT_H_ENTSIZE);
        first = gpt_le64(hdr + GPT_H_FIRST);
        last = gpt_le64(hdr + GPT_H_LAST);
        nparts = 0;
        if (backup && reg)
                printf("%s: the primary GPT is bad; using the backup\n",
                    disk->bd_name);
        for (uint32_t i = 0; i < nent && i < GPT_MAXENTRIES; i++) {
                const uint8_t *e = ents + (size_t)i * entsize;
                uint64_t start = gpt_le64(e + GPT_E_START);
                uint64_t end = gpt_le64(e + GPT_E_END);
                static const uint8_t unused[16];
                struct gpt_part *gp;
                char type[40], name[GPT_NAMELEN + 1];

                if (memcmp(e + GPT_E_TYPE, unused, 16) == 0)
                        continue;
                if (start < first || end > last || end < start) {
                        if (reg)
                                printf("%s: GPT entry %u is out of range\n",
                                    disk->bd_name, i + 1);
                        continue;
                }
                nparts++;
                if (!reg)
                        continue;
                gp = kmem_zalloc(sizeof(*gp), KM_SLEEP);
                snprintf(gp->gp_bd.bd_name, sizeof(gp->gp_bd.bd_name),
                    "%sp%u", disk->bd_name, i + 1);
                gp->gp_bd.bd_secsize = disk->bd_secsize;
                gp->gp_bd.bd_nsectors = end - start + 1;
                gp->gp_bd.bd_maxio = disk->bd_maxio;
                gp->gp_bd.bd_readonly = disk->bd_readonly;
                gp->gp_bd.bd_ops = &gpt_ops;
                gp->gp_bd.bd_softc = gp;
                gp->gp_bd.bd_parent = disk;
                gp->gp_bd.bd_start = start;
                gpt_typename(e, type, sizeof(type));
                gpt_name(e, name);
                printf("%s: GPT partition %u, sectors %ju-%ju, %s%s%s%s\n",
                    disk->bd_name, i + 1, (uintmax_t)start, (uintmax_t)end,
                    type, name[0] != '\0' ? " \"" : "", name,
                    name[0] != '\0' ? "\"" : "");
                if (blkdev_register_name(&gp->gp_bd) != 0) {
                        printf("%s: no room for its partitions\n",
                            disk->bd_name);
                        kmem_free(gp, sizeof(*gp));
                        break;
                }
        }
out:
        if (ents != NULL)
                kmem_free(ents, entlen);
        kmem_free(hdr, disk->bd_secsize);
        if (backupp != NULL)
                *backupp = backup;
        return nparts;
}

int
blkdev_gpt_probe(struct blkdev *disk, int *backup)
{
        return gpt_scan(disk, 0, backup);
}

void
blkdev_scan_partitions(void)
{
        struct blkdev *disks[BLKDEV_MAX];
        int n = 0;

        /* The disks there are now: the partitions follow them. */
        for (struct blkdev *bd; (bd = blkdev_index(n)) != NULL; n++)
                disks[n] = bd;
        for (int i = 0; i < n; i++)
                if (disks[i]->bd_parent == NULL)
                        (void)gpt_scan(disks[i], 1, NULL);
}
