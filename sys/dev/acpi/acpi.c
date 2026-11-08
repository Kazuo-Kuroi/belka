/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ACPI static table discovery. See <dev/acpi/acpivar.h>.
 *
 * Layouts are those of the ACPI specification:
 *
 *   RSDP:   "RSD PTR " at 0, checksum of the first 20 bytes at 8,
 *           revision at 15, RSDT address at 16; from revision 2, length
 *           at 20, XSDT address at 24, and a checksum of the whole
 *           structure at 32.
 *   Header: signature at 0, length at 4, checksum covering the whole
 *           table at 9, OEM id at 10.
 *   XSDT:   64-bit table addresses after the header; RSDT: 32-bit ones.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/systm.h>

#include <vm/hat.h>
#include <vm/kva.h>

#include "dev/acpi/acpivar.h"

struct acpi_table {
        char            at_sig[5];
        paddr_t         at_pa;
        size_t          at_len;
        const uint8_t   *at_va;
};

static struct acpi_table acpi_tables[ACPI_MAXTABLES];
static int acpi_ntables;
static int acpi_rev = -1;

static int acpi_checksum(const uint8_t *, size_t);
static const uint8_t *acpi_map_table(paddr_t, size_t *);

/* Nonzero if the bytes sum to zero, as every ACPI checksum requires. */
static int
acpi_checksum(const uint8_t *p, size_t len)
{
        uint8_t sum = 0;

        for (size_t i = 0; i < len; i++)
                sum = (uint8_t)(sum + p[i]);
        return sum == 0;
}

/*
 * Map the table at pa, reading its length from the header first.
 * Returns NULL if the length is implausible or the checksum is bad.
 */
static const uint8_t *
acpi_map_table(paddr_t pa, size_t *lenp)
{
        const uint8_t *hdr = kva_map_phys(pa, ACPI_HDR_LEN, HAT_READ);
        size_t len = acpi_le32(hdr + 4);
        const uint8_t *t;

        kva_unmap_phys((void *)(uintptr_t)hdr, ACPI_HDR_LEN);
        if (len < ACPI_HDR_LEN || len > 16 * 1024 * 1024)
                return NULL;
        t = kva_map_phys(pa, len, HAT_READ);
        if (!acpi_checksum(t, len)) {
                printf("acpi: table at %#jx (%.4s) has a bad checksum\n",
                    (uintmax_t)pa, (const char *)t);
                kva_unmap_phys((void *)(uintptr_t)t, len);
                return NULL;
        }
        *lenp = len;
        return t;
}

void
acpi_init(paddr_t rsdp_pa)
{
        const uint8_t *rsdp, *root;
        size_t root_len, entry_size;
        paddr_t root_pa;
        char names[ACPI_MAXTABLES * 5 + 1];
        size_t pos = 0;

        if (rsdp_pa == 0) {
                printf("acpi: no RSDP; ACPI unavailable\n");
                return;
        }
        rsdp = kva_map_phys(rsdp_pa, 36, HAT_READ);
        if (memcmp(rsdp, "RSD PTR ", 8) != 0 || !acpi_checksum(rsdp, 20)) {
                printf("acpi: RSDP at %#jx is invalid\n", (uintmax_t)rsdp_pa);
                kva_unmap_phys((void *)(uintptr_t)rsdp, 36);
                return;
        }
        acpi_rev = rsdp[15];
        if (acpi_rev >= 2 && acpi_le64(rsdp + 24) != 0 &&
            acpi_checksum(rsdp, acpi_le32(rsdp + 20))) {
                root_pa = acpi_le64(rsdp + 24);
                entry_size = 8;
        } else {
                root_pa = acpi_le32(rsdp + 16);
                entry_size = 4;
        }
        printf("acpi: revision %d, OEM \"%.6s\", %s at %#jx\n", acpi_rev,
            (const char *)rsdp + 9, entry_size == 8 ? "XSDT" : "RSDT",
            (uintmax_t)root_pa);
        kva_unmap_phys((void *)(uintptr_t)rsdp, 36);

        if ((root = acpi_map_table(root_pa, &root_len)) == NULL) {
                printf("acpi: root table is invalid\n");
                return;
        }
        for (size_t off = ACPI_HDR_LEN; off + entry_size <= root_len;
            off += entry_size) {
                paddr_t pa = entry_size == 8 ? acpi_le64(root + off) :
                    acpi_le32(root + off);
                struct acpi_table *at;
                const uint8_t *t;
                size_t len;

                if (acpi_ntables == ACPI_MAXTABLES) {
                        printf("acpi: more than %d tables; rest ignored\n",
                            ACPI_MAXTABLES);
                        break;
                }
                if (pa == 0 || (t = acpi_map_table(pa, &len)) == NULL)
                        continue;
                at = &acpi_tables[acpi_ntables++];
                memcpy(at->at_sig, t, 4);
                at->at_sig[4] = '\0';
                at->at_pa = pa;
                at->at_len = len;
                at->at_va = t;
                memcpy(names + pos, t, 4);
                names[pos + 4] = ' ';
                pos += 5;
        }
        names[pos > 0 ? pos - 1 : 0] = '\0';
        printf("acpi: tables: %s\n", names);
}

int
acpi_present(void)
{
        return acpi_ntables > 0;
}

int
acpi_revision(void)
{
        return acpi_rev;
}

const uint8_t *
acpi_find_table(const char *sig, int index, size_t *lenp)
{
        for (int i = 0; i < acpi_ntables; i++) {
                if (memcmp(acpi_tables[i].at_sig, sig, 4) != 0)
                        continue;
                if (index-- > 0)
                        continue;
                if (lenp != NULL)
                        *lenp = acpi_tables[i].at_len;
                return acpi_tables[i].at_va;
        }
        return NULL;
}
