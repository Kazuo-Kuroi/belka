/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Information handed over by the boot loader, in a form independent of
 * the boot protocol. Only limine_boot.c knows about Limine; the rest of
 * the kernel reads this structure.
 */

#ifndef _MACHINE_BOOTINFO_H_
#define _MACHINE_BOOTINFO_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/param.h>

#define BOOTMEM_MAX     128     /* memory map entries kept */

enum bootmem_type {
        BOOTMEM_USABLE,
        BOOTMEM_RESERVED,
        BOOTMEM_ACPI_RECLAIM,
        BOOTMEM_ACPI_NVS,
        BOOTMEM_BAD,
        BOOTMEM_LOADER_RECLAIM, /* free once boot loader data is consumed */
        BOOTMEM_KERNEL,         /* kernel image and boot modules */
        BOOTMEM_FRAMEBUFFER,
        BOOTMEM_OTHER
};

struct bootmem {
        uint64_t        bm_base;
        uint64_t        bm_length;
        enum bootmem_type bm_type;
};

#define BOOTINFO_NAMELEN 64
#define BOOTINFO_CMDLEN 256

struct bootfb {
        uint64_t        bf_pa;
        uint64_t        bf_width;
        uint64_t        bf_height;
        uint64_t        bf_pitch;       /* bytes */
        uint8_t         bf_red;
        uint8_t         bf_green;
        uint8_t         bf_blue;
};

/*
 * Everything here is copied out of boot loader memory, so it stays valid
 * after that memory is reclaimed.
 */
struct bootinfo {
        char            bi_loader_name[BOOTINFO_NAMELEN];
        char            bi_loader_version[BOOTINFO_NAMELEN];
        char            bi_cmdline[BOOTINFO_CMDLEN];
        uint64_t        bi_hhdm_offset; /* VA of physical 0 in the direct map */
        uint64_t        bi_kernel_pa;   /* physical load address */
        uint64_t        bi_kernel_va;   /* virtual load address */
        uint64_t        bi_rsdp;        /* ACPI RSDP physical address, or 0 */
        struct bootfb   bi_fb;          /* 32-bit RGB888, or bf_pa == 0 */
        uint64_t        bi_ncpu;        /* processors listed, at most MAXCPUS */
        uint64_t        bi_ncpu_dropped; /* processors beyond MAXCPUS */
        uint32_t        bi_bsp_lapic_id;
        uint32_t        bi_cpu_lapic[MAXCPUS]; /* in boot loader order */
        uint32_t        bi_nmem;        /* entries used in bi_mem */
        uint32_t        bi_mem_dropped; /* entries that did not fit */
        struct bootmem  bi_mem[BOOTMEM_MAX];
};

extern struct bootinfo bootinfo;

/* Nonzero for memory types that are RAM rather than device space. */
static inline int
bootmem_is_ram(enum bootmem_type type)
{
        switch (type) {
        case BOOTMEM_USABLE:
        case BOOTMEM_ACPI_RECLAIM:
        case BOOTMEM_ACPI_NVS:
        case BOOTMEM_LOADER_RECLAIM:
        case BOOTMEM_KERNEL:
                return 1;
        default:
                return 0;
        }
}

/*
 * Fill in bootinfo from the boot loader's responses. Returns 0 on
 * success, or -1 if the loader does not support the protocol revision
 * the kernel was built for.
 */
int limine_boot_collect(struct bootinfo *);

/*
 * Start the processor at index i of bi_cpu_lapic, which must not be the
 * bootstrap processor. It calls fn(arg) on a small boot loader stack
 * with interrupts disabled, and fn must not return.
 */
void limine_cpu_start(unsigned int, void (*)(void *), void *);

/*
 * Forget every pointer into boot loader memory, before that memory is
 * handed to the page allocator. No processor can be started afterwards.
 */
void limine_boot_release(void);

/* Return nonzero if word appears as a whole word in the command line. */
int boot_option(const char *);

/* The value of a "name=value" word in the command line, if there is one. */
int boot_option_value(const char *, char *, size_t);

#endif /* _MACHINE_BOOTINFO_H_ */
