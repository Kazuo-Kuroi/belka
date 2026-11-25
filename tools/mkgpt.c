/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * Write a GUID partition table into an existing disk image, in place:
 * a protective MBR in sector 0, the primary header in sector 1 and its
 * 128 entries in sectors 2-33, the backup entries in the 32 sectors
 * before the last, and the backup header in the last. Nothing else in
 * the image changes, so a partition's contents may be written first.
 * Sectors are 512 bytes. The GUIDs are made from the image's size and
 * the entry numbers, so the same command makes the same table.
 *
 * usage: mkgpt image [-n entry] start,end,type[,name] ...
 *   start, end  the partition's first and last sectors
 *   type        efi, bios, linux, swap or basic
 *   -n entry    put the next partition in that entry (from 1), not the
 *               next one in order
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECSIZE         512
#define NENTRIES        128
#define ENTSIZE         128
#define ARRAYSECS       (NENTRIES * ENTSIZE / SECSIZE)

static const struct {
        const char      *name;
        uint8_t         guid[16];       /* as on disk */
} types[] = {
        { "efi", { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11, 0xba, 0x4b,
            0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } },
        { "bios", { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e, 0x74,
            0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 } },
        { "linux", { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e,
            0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 } },
        { "swap", { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43, 0x84,
            0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f } },
        { "basic", { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44, 0x87,
            0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } },
};

static uint32_t
crc32(const uint8_t *p, size_t len)
{
        uint32_t crc = 0xffffffffu;

        while (len-- > 0) {
                crc ^= *p++;
                for (int k = 0; k < 8; k++)
                        crc = (crc & 1) != 0 ? 0xedb88320u ^ (crc >> 1) :
                            crc >> 1;
        }
        return ~crc;
}

static void
put32(uint8_t *p, uint32_t v)
{
        for (int i = 0; i < 4; i++)
                p[i] = (uint8_t)(v >> (8 * i));
}

static void
put64(uint8_t *p, uint64_t v)
{
        put32(p, (uint32_t)v);
        put32(p + 4, (uint32_t)(v >> 32));
}

/* A version 4 GUID from seed. */
static void
guid(uint8_t *g, uint64_t seed)
{
        for (int i = 0; i < 16; i++) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                g[i] = (uint8_t)(seed >> 56);
        }
        g[7] = (uint8_t)((g[7] & 0x0f) | 0x40);
        g[8] = (uint8_t)((g[8] & 0x3f) | 0x80);
}

static int
put(FILE *f, uint64_t lba, const void *buf, size_t nsec)
{
        return fseek(f, (long)(lba * SECSIZE), SEEK_SET) == 0 &&
            fwrite(buf, SECSIZE, nsec, f) == nsec;
}

static void
header(uint8_t *h, uint64_t my, uint64_t alt, uint64_t first, uint64_t last,
    const uint8_t *diskguid, uint64_t entlba, uint32_t arraycrc)
{
        memset(h, 0, SECSIZE);
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, my);
        put64(h + 32, alt);
        put64(h + 40, first);
        put64(h + 48, last);
        memcpy(h + 56, diskguid, 16);
        put64(h + 72, entlba);
        put32(h + 80, NENTRIES);
        put32(h + 84, ENTSIZE);
        put32(h + 88, arraycrc);
        put32(h + 16, crc32(h, 92));
}

int
main(int argc, char **argv)
{
        static uint8_t array[NENTRIES * ENTSIZE];
        uint8_t mbr[SECSIZE], hdr[SECSIZE], diskguid[16];
        uint64_t nsec, last, first = 2 + ARRAYSECS, lastusable;
        unsigned int next = 0;
        uint32_t arraycrc;
        long size;
        FILE *f;

        if (argc < 2) {
                fprintf(stderr, "usage: %s image [-n entry] "
                    "start,end,type[,name] ...\n", argv[0]);
                return 2;
        }
        if ((f = fopen(argv[1], "r+b")) == NULL || fseek(f, 0, SEEK_END) != 0 ||
            (size = ftell(f)) < 0) {
                perror(argv[1]);
                return 1;
        }
        nsec = (uint64_t)size / SECSIZE;
        if (nsec < 2 * first + 1) {
                fprintf(stderr, "%s: too small for a GPT\n", argv[1]);
                return 1;
        }
        last = nsec - 1;
        lastusable = last - 1 - ARRAYSECS;
        guid(diskguid, nsec);

        for (int i = 2; i < argc; i++) {
                char *spec, *s, *e, *t, *name;
                unsigned long long start, end;
                uint8_t *ent;
                size_t k;

                if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
                        next = (unsigned int)strtoul(argv[++i], NULL, 10) - 1;
                        continue;
                }
                spec = strdup(argv[i]);
                s = strtok(spec, ",");
                e = strtok(NULL, ",");
                t = strtok(NULL, ",");
                name = strtok(NULL, "");
                if (s == NULL || e == NULL || t == NULL) {
                        fprintf(stderr, "%s: bad partition %s\n", argv[0],
                            argv[i]);
                        return 2;
                }
                start = strtoull(s, NULL, 0);
                end = strtoull(e, NULL, 0);
                for (k = 0; k < sizeof(types) / sizeof(types[0]); k++)
                        if (strcmp(types[k].name, t) == 0)
                                break;
                if (k == sizeof(types) / sizeof(types[0]) || next >= NENTRIES ||
                    start < first || end > lastusable || end < start) {
                        fprintf(stderr, "%s: bad partition %s\n", argv[0],
                            argv[i]);
                        return 2;
                }
                ent = array + next * ENTSIZE;
                memcpy(ent, types[k].guid, 16);
                guid(ent + 16, nsec * 131 + next);
                put64(ent + 32, start);
                put64(ent + 40, end);
                for (size_t c = 0; name != NULL && name[c] != '\0' && c < 36;
                    c++)
                        ent[56 + 2 * c] = (uint8_t)name[c];
                next++;
                free(spec);
        }
        arraycrc = crc32(array, sizeof(array));

        /* The protective MBR: one partition of type 0xee over the disk. */
        memset(mbr, 0, sizeof(mbr));
        mbr[446 + 2] = 0x02;                    /* CHS 0/0/2 */
        mbr[446 + 4] = 0xee;
        mbr[446 + 5] = mbr[446 + 6] = mbr[446 + 7] = 0xff;
        put32(mbr + 446 + 8, 1);
        put32(mbr + 446 + 12, last > 0xffffffffu ? 0xffffffffu :
            (uint32_t)last);
        mbr[510] = 0x55;
        mbr[511] = 0xaa;

        if (!put(f, 0, mbr, 1) || !put(f, 2, array, ARRAYSECS) ||
            !put(f, last - ARRAYSECS, array, ARRAYSECS)) {
                perror(argv[1]);
                return 1;
        }
        header(hdr, 1, last, first, lastusable, diskguid, 2, arraycrc);
        if (!put(f, 1, hdr, 1)) {
                perror(argv[1]);
                return 1;
        }
        header(hdr, last, 1, first, lastusable, diskguid, last - ARRAYSECS,
            arraycrc);
        if (!put(f, last, hdr, 1) || fclose(f) != 0) {
                perror(argv[1]);
                return 1;
        }
        return 0;
}
