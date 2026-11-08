/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * ACPI static tables.
 *
 * acpi_init() validates the RSDP, walks the XSDT (or the RSDT on ACPI 1.0
 * firmware), and maps every table whose checksum is good. Tables stay
 * mapped, read-only, for the life of the system. There is no AML
 * interpreter: only the fixed-format tables are used.
 *
 * Table fields are read with the little-endian helpers below rather
 * than through structure overlays, because many fields are unaligned
 * and the layouts cannot be described in C without packing.
 */

#ifndef _DEV_ACPI_ACPIVAR_H_
#define _DEV_ACPI_ACPIVAR_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/types.h>

#define ACPI_HDR_LEN            36      /* common table header */
#define ACPI_MAXTABLES          64

static inline uint16_t
acpi_le16(const uint8_t *p)
{
        return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t
acpi_le32(const uint8_t *p)
{
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t
acpi_le64(const uint8_t *p)
{
        return (uint64_t)acpi_le32(p) | ((uint64_t)acpi_le32(p + 4) << 32);
}

void acpi_init(paddr_t);
int acpi_present(void);

/*
 * Return the index'th table with signature sig (four characters), or
 * NULL. Its length is stored in *lenp if lenp is not NULL.
 */
const uint8_t *acpi_find_table(const char *, int, size_t *);

/* The revision of the RSDP: 0 for ACPI 1.0, 2 or more with an XSDT. */
int acpi_revision(void);

#endif /* _DEV_ACPI_ACPIVAR_H_ */
