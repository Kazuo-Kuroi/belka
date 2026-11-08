/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 machine-dependent startup and shutdown.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/systm.h>
#include <sys/uadmin.h>

#include <machine/bootinfo.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>

#include "dev/ic/ns16550var.h"

#define CONSOLE_IOBASE  0x3f8   /* COM1 */
#define CONSOLE_SPEED   115200

/*
 * The QEMU isa-debug-exit device. Writing v makes QEMU exit with status
 * (v << 1) | 1. It is touched only when the "debugexit" boot option is
 * given, because the port may belong to something else on real hardware.
 */
#define DEBUG_EXIT_PORT 0xf4
#define DEBUG_EXIT_PASS 0x10    /* QEMU exits with 33 */
#define DEBUG_EXIT_FAIL 0x11    /* QEMU exits with 35 */

struct bootinfo bootinfo;

static struct ns16550_softc console_softc;

static const char *const bootmem_names[] = {
        "usable",
        "reserved",
        "ACPI reclaimable",
        "ACPI NVS",
        "bad",
        "loader reclaimable",
        "kernel",
        "framebuffer",
        "other"
};

static void print_memory_map(const struct bootinfo *);

static void
print_memory_map(const struct bootinfo *bi)
{
        uint64_t usable = 0;

        printf("physical memory map:\n");
        for (uint32_t i = 0; i < bi->bi_nmem; i++) {
                const struct bootmem *bm = &bi->bi_mem[i];

                printf("  %016jx-%016jx %s\n",
                    (uintmax_t)bm->bm_base,
                    (uintmax_t)(bm->bm_base + bm->bm_length - 1),
                    bootmem_names[bm->bm_type]);
                if (bm->bm_type == BOOTMEM_USABLE)
                        usable += bm->bm_length;
        }
        if (bi->bi_mem_dropped != 0)
                printf("  (%u entries not recorded)\n", bi->bi_mem_dropped);
        printf("usable memory: %ju KB\n", (uintmax_t)(usable / 1024));
}

/*
 * First C code on the bootstrap processor. Interrupts are disabled and
 * the boot loader's page tables and GDT are still in use.
 */
void
amd64_init(void)
{
        struct bootinfo *bi = &bootinfo;
        int collected;

        (void)ns16550_cninit(&console_softc, CONSOLE_IOBASE, CONSOLE_SPEED);

        collected = limine_boot_collect(bi);
        if (collected != 0)
                panic("boot loader does not support Limine base revision 6");

        printf("boot loader: %s %s\n",
            bi->bi_loader_name != NULL ? bi->bi_loader_name : "unknown",
            bi->bi_loader_version != NULL ? bi->bi_loader_version : "");
        printf("command line: \"%s\"\n",
            bi->bi_cmdline != NULL ? bi->bi_cmdline : "");
        printf("kernel: va %#jx pa %#jx, %ju KB\n",
            (uintmax_t)bi->bi_kernel_va, (uintmax_t)bi->bi_kernel_pa,
            (uintmax_t)((size_t)(kernel_end - kernel_start) / 1024));
        printf("direct map at %p\n", (void *)(uintptr_t)bi->bi_hhdm_offset);
        printf("ACPI RSDP at %p\n", (void *)(uintptr_t)bi->bi_rsdp);
        printf("processors: %ju (bootstrap LAPIC id %u)\n",
            (uintmax_t)bi->bi_ncpu, bi->bi_bsp_lapic_id);
        print_memory_map(bi);

        main();
}

int
boot_option(const char *word)
{
        const char *p = bootinfo.bi_cmdline;
        size_t len = strlen(word);

        if (p == NULL || len == 0)
                return 0;
        while (*p != '\0') {
                while (*p == ' ')
                        p++;
                if (strncmp(p, word, len) == 0 &&
                    (p[len] == ' ' || p[len] == '\0'))
                        return 1;
                while (*p != ' ' && *p != '\0')
                        p++;
        }
        return 0;
}

/*
 * Stop this processor. Rebooting is not implemented yet, so every
 * function halts. Other processors are not running yet.
 */
void
mdboot(int fcn, int status)
{
        static int reported;

        if (fcn != AD_HALT)
                printf("mdboot: reboot not implemented, halting\n");

        printf("halted\n");

        if (reported++ == 0 && boot_option("debugexit"))
                outb(DEBUG_EXIT_PORT,
                    status == 0 ? DEBUG_EXIT_PASS : DEBUG_EXIT_FAIL);

        for (;;) {
                disable_intr();
                cpu_halt();
        }
}
