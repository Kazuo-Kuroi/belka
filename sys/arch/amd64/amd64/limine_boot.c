/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Limine boot protocol requests.
 *
 * The loader finds requests by scanning the kernel image for their
 * magic numbers, and writes response pointers into them before entry.
 * All requests sit in one structure between the start and end markers.
 * A structure is used, rather than separate objects, because C fixes
 * the order of its members but not the order of separate objects, and
 * because it needs no section attributes to keep the requests together.
 *
 * The object is volatile because the loader writes it behind the
 * compiler's back.
 */

#include <stddef.h>
#include <stdint.h>

#include <limine/limine.h>

#include <machine/bootinfo.h>

#define BELKA_LIMINE_REVISION   6

static volatile struct {
        uint64_t        start_marker[4];
        uint64_t        base_revision[3];
        struct limine_bootloader_info_request info;
        struct limine_executable_cmdline_request cmdline;
        struct limine_hhdm_request hhdm;
        struct limine_memmap_request memmap;
        struct limine_executable_address_request kaddr;
        struct limine_rsdp_request rsdp;
        struct limine_mp_request mp;
        uint64_t        end_marker[2];
} limine_requests = {
        LIMINE_REQUESTS_START_MARKER,
        LIMINE_BASE_REVISION(BELKA_LIMINE_REVISION),
        { LIMINE_BOOTLOADER_INFO_REQUEST_ID, 0, NULL },
        { LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, 0, NULL },
        { LIMINE_HHDM_REQUEST_ID, 0, NULL },
        { LIMINE_MEMMAP_REQUEST_ID, 0, NULL },
        { LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, 0, NULL },
        { LIMINE_RSDP_REQUEST_ID, 0, NULL },
        { LIMINE_MP_REQUEST_ID, 0, NULL, 0 },
        LIMINE_REQUESTS_END_MARKER
};

static enum bootmem_type
limine_memtype(uint64_t type)
{
        switch (type) {
        case LIMINE_MEMMAP_USABLE:
                return BOOTMEM_USABLE;
        case LIMINE_MEMMAP_RESERVED:
        case LIMINE_MEMMAP_RESERVED_MAPPED:
                return BOOTMEM_RESERVED;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
                return BOOTMEM_ACPI_RECLAIM;
        case LIMINE_MEMMAP_ACPI_NVS:
                return BOOTMEM_ACPI_NVS;
        case LIMINE_MEMMAP_BAD_MEMORY:
                return BOOTMEM_BAD;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
                return BOOTMEM_LOADER_RECLAIM;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
                return BOOTMEM_KERNEL;
        case LIMINE_MEMMAP_FRAMEBUFFER:
                return BOOTMEM_FRAMEBUFFER;
        default:
                return BOOTMEM_OTHER;
        }
}

int
limine_boot_collect(struct bootinfo *bi)
{
        struct limine_bootloader_info_response *info;
        struct limine_executable_cmdline_response *cmdline;
        struct limine_hhdm_response *hhdm;
        struct limine_memmap_response *memmap;
        struct limine_executable_address_response *kaddr;
        struct limine_rsdp_response *rsdp;
        struct limine_mp_response *mp;

        /* The loader zeroes the third word when it accepts the revision. */
        if (limine_requests.base_revision[2] != 0)
                return -1;

        if ((info = limine_requests.info.response) != NULL) {
                bi->bi_loader_name = info->name;
                bi->bi_loader_version = info->version;
        }
        if ((cmdline = limine_requests.cmdline.response) != NULL)
                bi->bi_cmdline = cmdline->cmdline;
        if ((hhdm = limine_requests.hhdm.response) != NULL)
                bi->bi_hhdm_offset = hhdm->offset;
        if ((kaddr = limine_requests.kaddr.response) != NULL) {
                bi->bi_kernel_pa = kaddr->physical_base;
                bi->bi_kernel_va = kaddr->virtual_base;
        }
        if ((rsdp = limine_requests.rsdp.response) != NULL)
                bi->bi_rsdp = (uint64_t)(uintptr_t)rsdp->address;
        if ((mp = limine_requests.mp.response) != NULL) {
                bi->bi_ncpu = mp->cpu_count;
                bi->bi_bsp_lapic_id = mp->bsp_lapic_id;
        } else {
                bi->bi_ncpu = 1;
        }

        if ((memmap = limine_requests.memmap.response) != NULL) {
                for (uint64_t i = 0; i < memmap->entry_count; i++) {
                        struct limine_memmap_entry *e = memmap->entries[i];
                        struct bootmem *bm;

                        if (bi->bi_nmem == BOOTMEM_MAX) {
                                bi->bi_mem_dropped++;
                                continue;
                        }
                        bm = &bi->bi_mem[bi->bi_nmem++];
                        bm->bm_base = e->base;
                        bm->bm_length = e->length;
                        bm->bm_type = limine_memtype(e->type);
                }
        }

        return 0;
}
