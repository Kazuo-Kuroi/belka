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

#include <sys/systm.h>

#include <limine/limine.h>

#include <machine/atomic.h>
#include <machine/bootinfo.h>

#define BELKA_LIMINE_REVISION   6

static volatile struct {
        uint64_t        start_marker[4];
        uint64_t        base_revision[3];
        struct limine_bootloader_info_request info;
        struct limine_executable_cmdline_request cmdline;
        struct limine_hhdm_request hhdm;
        struct limine_memmap_request memmap;
        struct limine_framebuffer_request fb;
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
        { LIMINE_FRAMEBUFFER_REQUEST_ID, 0, NULL },
        { LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, 0, NULL },
        { LIMINE_RSDP_REQUEST_ID, 0, NULL },
        { LIMINE_MP_REQUEST_ID, 0, NULL, 0 },
        LIMINE_REQUESTS_END_MARKER
};

static struct limine_mp_response *limine_mp;
static void (*limine_cpu_fn)(void *);

static void limine_cpu_entry(struct limine_mp_info *);

static void limine_strcpy(char *, const char *, size_t);

static void
limine_fb_collect(struct bootinfo *bi)
{
        const struct limine_framebuffer_response *r =
            limine_requests.fb.response;

        if (r == NULL || bi->bi_hhdm_offset == 0)
                return;
        for (uint64_t i = 0; i < r->framebuffer_count; i++) {
                const struct limine_framebuffer *f = r->framebuffers[i];
                uint64_t va = (uint64_t)(uintptr_t)f->address;

                if (va <= bi->bi_hhdm_offset || f->width < 640 ||
                    f->height < 384 || f->memory_model != LIMINE_FRAMEBUFFER_RGB ||
                    f->bpp != 32 || f->red_mask_size != 8 ||
                    f->green_mask_size != 8 || f->blue_mask_size != 8 ||
                    f->red_mask_shift > 24 || f->green_mask_shift > 24 ||
                    f->blue_mask_shift > 24 ||
                    ((f->red_mask_shift | f->green_mask_shift |
                    f->blue_mask_shift) & 7) != 0 ||
                    f->red_mask_shift == f->green_mask_shift ||
                    f->red_mask_shift == f->blue_mask_shift ||
                    f->green_mask_shift == f->blue_mask_shift)
                        continue;
                bi->bi_fb = (struct bootfb) {
                        va - bi->bi_hhdm_offset, f->width, f->height,
                        f->pitch, f->red_mask_shift, f->green_mask_shift,
                        f->blue_mask_shift
                };
                return;
        }
}

/* Copy a string from loader memory, truncating to fit. */
static void
limine_strcpy(char *dst, const char *src, size_t size)
{
        size_t i = 0;

        if (src != NULL) {
                for (; i + 1 < size && src[i] != '\0'; i++)
                        dst[i] = src[i];
        }
        dst[i] = '\0';
}

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
                limine_strcpy(bi->bi_loader_name, info->name,
                    sizeof(bi->bi_loader_name));
                limine_strcpy(bi->bi_loader_version, info->version,
                    sizeof(bi->bi_loader_version));
        }
        if ((cmdline = limine_requests.cmdline.response) != NULL)
                limine_strcpy(bi->bi_cmdline, cmdline->cmdline,
                    sizeof(bi->bi_cmdline));
        if ((hhdm = limine_requests.hhdm.response) != NULL)
                bi->bi_hhdm_offset = hhdm->offset;
        limine_fb_collect(bi);
        if ((kaddr = limine_requests.kaddr.response) != NULL) {
                bi->bi_kernel_pa = kaddr->physical_base;
                bi->bi_kernel_va = kaddr->virtual_base;
        }
        /* From base revision 3 the RSDP is given as a direct-map address. */
        if ((rsdp = limine_requests.rsdp.response) != NULL &&
            rsdp->address != NULL)
                bi->bi_rsdp = (uint64_t)(uintptr_t)rsdp->address -
                    bi->bi_hhdm_offset;
        if ((mp = limine_requests.mp.response) != NULL) {
                limine_mp = mp;
                bi->bi_bsp_lapic_id = mp->bsp_lapic_id;
                bi->bi_ncpu = 0;
                for (uint64_t i = 0; i < mp->cpu_count; i++) {
                        if (bi->bi_ncpu == MAXCPUS) {
                                bi->bi_ncpu_dropped++;
                                continue;
                        }
                        bi->bi_cpu_lapic[bi->bi_ncpu++] =
                            mp->cpus[i]->lapic_id;
                }
        } else {
                bi->bi_ncpu = 1;
                bi->bi_cpu_lapic[0] = 0;
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

/* First code run by a secondary processor started by Limine. */
static void
limine_cpu_entry(struct limine_mp_info *info)
{
        limine_cpu_fn((void *)(uintptr_t)info->extra_argument);
        panic("limine_cpu_entry: start function returned");
}

void
limine_boot_release(void)
{
        limine_mp = NULL;
}

void
limine_cpu_start(unsigned int i, void (*fn)(void *), void *arg)
{
        struct limine_mp_info *info;

        if (limine_mp == NULL || i >= limine_mp->cpu_count)
                panic("limine_cpu_start: no processor %u", i);
        info = limine_mp->cpus[i];
        if (info->lapic_id == limine_mp->bsp_lapic_id)
                panic("limine_cpu_start: processor %u is the BSP", i);

        /*
         * The processor polls goto_address, so everything it reads must
         * be in place before that store. amd64 keeps stores in order, so
         * compiler barriers on both sides are enough. The aligned 64-bit
         * store is a single instruction, which the processor sees whole.
         */
        limine_cpu_fn = fn;
        info->extra_argument = (uint64_t)(uintptr_t)arg;
        membar_producer();
        info->goto_address = limine_cpu_entry;
        membar_producer();
}
