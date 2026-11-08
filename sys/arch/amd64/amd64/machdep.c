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

#include <sys/cons.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/uadmin.h>

#include <machine/atomic.h>
#include <machine/bootinfo.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <machine/trap.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/page.h>

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

/* An address no boot loader maps, for the deliberate-fault test. */
#define UNMAPPED_VA     UINT64_C(0x00007fffdead0000)

struct bootinfo bootinfo;
vaddr_t dmap_base;

/* Interrupt stacks for the bootstrap processor. */
static uint64_t bsp_ist_stacks[NIST][IST_STACK_SIZE / sizeof(uint64_t)];

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
static uint64_t stack_top(const void *, size_t);
static void md_fault_on_cpu1(void *);
static int md_expect_read_fault(uint64_t, uint64_t);
static int md_expect_write_fault(uint64_t, uint64_t, uint64_t);
static void md_kva_read_all(void *);
static void md_kva_fault_all(void *);
static int md_selftest_hat(void);
static void cpu_halt_self_forever(void) __dead;

static void
cpu_halt_self_forever(void)
{
        for (;;) {
                disable_intr();
                cpu_halt();
        }
}

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

/* Return the 16-byte aligned top of a stack of size bytes at base. */
static uint64_t
stack_top(const void *base, size_t size)
{
        return ((uint64_t)(uintptr_t)base + size) & ~(uint64_t)15;
}

/*
 * First C code on the bootstrap processor. Interrupts are disabled and
 * the boot loader's page tables and GDT are still in use.
 */
void
amd64_init(void)
{
        struct bootinfo *bi = &bootinfo;
        struct cpu_info *ci = &cpu_info_primary;
        int collected;

        /*
         * Take over the descriptor tables and the GS base first. After
         * this, faults are reported instead of resetting the machine,
         * and curcpu() works, which spin locks (and so printf) need.
         */
        ci->ci_cpuid = 0;
        ci->ci_flags = CPUF_PRESENT | CPUF_BSP | CPUF_RUNNING;
        ci->ci_stack_top = stack_top(boot_stack,
            (size_t)(boot_stack_top - boot_stack));
        for (int i = 0; i < NIST; i++)
                ci->ci_ist_top[i] = stack_top(bsp_ist_stacks[i],
                    sizeof(bsp_ist_stacks[i]));
        idt_init();
        cpu_tables_init(ci);

        (void)ns16550_cninit(&console_softc, CONSOLE_IOBASE, CONSOLE_SPEED);

        collected = limine_boot_collect(bi);
        if (collected != 0)
                panic("boot loader does not support Limine base revision 6");

        printf("boot loader: %s %s\n",
            bi->bi_loader_name[0] != '\0' ? bi->bi_loader_name : "unknown",
            bi->bi_loader_version);
        printf("command line: \"%s\"\n", bi->bi_cmdline);
        printf("kernel: va %#jx pa %#jx, %ju KB\n",
            (uintmax_t)bi->bi_kernel_va, (uintmax_t)bi->bi_kernel_pa,
            (uintmax_t)((size_t)(kernel_end - kernel_start) / 1024));
        printf("direct map at %p\n", (void *)(uintptr_t)bi->bi_hhdm_offset);
        printf("ACPI RSDP at physical %#jx\n", (uintmax_t)bi->bi_rsdp);
        printf("processors: %ju (bootstrap LAPIC id %u)\n",
            (uintmax_t)bi->bi_ncpu, bi->bi_bsp_lapic_id);
        ci->ci_lapic_id = bi->bi_bsp_lapic_id;
        print_memory_map(bi);

        /*
         * Give usable memory to the page allocator. Memory the boot
         * loader marks reclaimable holds its page tables and the other
         * processors' stacks, so it is deferred until bootloader_reclaim().
         */
        dmap_base = bi->bi_hhdm_offset;
        for (uint32_t i = 0; i < bi->bi_nmem; i++) {
                const struct bootmem *bm = &bi->bi_mem[i];

                if (bm->bm_type == BOOTMEM_USABLE)
                        page_seg_add(bm->bm_base,
                            bm->bm_base + bm->bm_length, 0, 0);
                else if (bm->bm_type == BOOTMEM_LOADER_RECLAIM)
                        page_seg_add(bm->bm_base,
                            bm->bm_base + bm->bm_length, 0, PSEG_DEFER);
        }

        main();
}

/* Touch an unmapped address on processor 1 only. */
static void
md_fault_on_cpu1(void *arg __unused)
{
        if (cpu_number() == 1)
                *(volatile uint64_t *)(uintptr_t)UNMAPPED_VA = 1;
}

/* Test data for the protection checks. */
static const uint64_t md_rodata_word = UINT64_C(0x5eed5eed5eed5eed);
static uint64_t md_data_word = 1;
static uint8_t md_nx_code[16] = { 0xc3 };      /* ret */

static volatile uint64_t md_kva_probe_va;
static volatile uint32_t md_kva_reads_ok;
static volatile uint32_t md_kva_faults;

/*
 * Expect the access to fault with a page fault. want_err holds the error
 * code bits that must be set. Returns 1 if it did.
 */
static int
md_expect_read_fault(uint64_t addr, uint64_t want_err)
{
        uint64_t err;

        trap_expect(PGFLT, (int)(cpu_read_insn_end - cpu_read_insn));
        (void)cpu_read_probe((const volatile uint64_t *)(uintptr_t)addr);
        return trap_expect_done(&err) && (err & want_err) == want_err;
}

static int
md_expect_write_fault(uint64_t addr, uint64_t value, uint64_t want_err)
{
        uint64_t err;

        trap_expect(PGFLT, (int)(cpu_write_insn_end - cpu_write_insn));
        cpu_write_probe((volatile uint64_t *)(uintptr_t)addr, value);
        return trap_expect_done(&err) && (err & want_err) == want_err;
}

/* Each processor reads the probe page, loading it into its TLB. */
static void
md_kva_read_all(void *arg __unused)
{
        const volatile uint64_t *p =
            (const volatile uint64_t *)(uintptr_t)md_kva_probe_va;

        if (*p == UINT64_C(0xfeedface))
                atomic_inc_32_nv(&md_kva_reads_ok);
}

/* After hat_kremove(), every processor must fault on the probe page. */
static void
md_kva_fault_all(void *arg __unused)
{
        if (md_expect_read_fault(md_kva_probe_va, 0))
                atomic_inc_32_nv(&md_kva_faults);
}

/* Kernel page table checks; returns the number of failures. */
static int
md_selftest_hat(void)
{
        int failures = 0;
        struct page *pp;
        paddr_t pa;
        vaddr_t va;

#define HCHECK(expr)                                                    \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

        /* The low half is empty. */
        HCHECK(md_expect_read_fault(0, 0));
        HCHECK(md_expect_read_fault(UINT64_C(0x1000), 0));

        /* Text and read-only data refuse writes; data accepts them. */
        HCHECK(md_expect_write_fault((uint64_t)(uintptr_t)kernel_start,
            *(const volatile uint64_t *)(void *)kernel_start,
            PGEX_P | PGEX_W));
        HCHECK(md_expect_write_fault((uint64_t)(uintptr_t)&md_rodata_word,
            md_rodata_word, PGEX_P | PGEX_W));
        cpu_write_probe(&md_data_word, 2);
        HCHECK(md_data_word == 2);

        /* Data is not executable. */
        if (hat_nx_enabled()) {
                uint64_t err;

                trap_expect(PGFLT, TRAP_RESUME_RETURN);
                cpu_call_probe(md_nx_code);
                HCHECK(trap_expect_done(&err) &&
                    (err & (PGEX_P | PGEX_I)) == (PGEX_P | PGEX_I));
        }

        /* Translation. */
        HCHECK(hat_kextract((vaddr_t)(uintptr_t)kernel_start, &pa) == 0);
        HCHECK(hat_kextract(dmap_base + UINT64_C(0x200000), &pa) == 0 &&
            pa == UINT64_C(0x200000));
        HCHECK(hat_kextract(KVA_BASE, &pa) != 0);

        /* KVA mappings, and their removal from every processor's TLB. */
        if ((pp = page_alloc(0, PA_ZERO)) == NULL) {
                HCHECK(pp != NULL);
                return failures;
        }
        va = KVA_BASE + 16 * PAGESIZE;
        HCHECK(hat_kenter(va, page_to_pa(pp), HAT_WRITE) == 0);
        HCHECK(hat_kenter(va, page_to_pa(pp), HAT_WRITE) != 0);
        HCHECK(hat_kextract(va, &pa) == 0 && pa == page_to_pa(pp));
        *(volatile uint64_t *)(uintptr_t)va = UINT64_C(0xfeedface);
        HCHECK(*(volatile uint64_t *)page_to_kva(pp) ==
            UINT64_C(0xfeedface));

        md_kva_probe_va = va;
        md_kva_reads_ok = 0;
        md_kva_faults = 0;
        smp_call_all(md_kva_read_all, NULL);
        HCHECK(md_kva_reads_ok == ncpus);
        hat_kremove(va, 1);
        HCHECK(hat_kextract(va, &pa) != 0);
        smp_call_all(md_kva_fault_all, NULL);
        HCHECK(md_kva_faults == ncpus);

        /* A read-only KVA mapping refuses writes. */
        HCHECK(hat_kenter(va, page_to_pa(pp), HAT_READ) == 0);
        HCHECK(md_expect_write_fault(va, 0, PGEX_P | PGEX_W));
        hat_kremove(va, 1);

        page_free(pp, 0);
        return failures;
#undef HCHECK
}

/*
 * amd64 self-tests: per-processor data, exception delivery, and the
 * kernel page tables. With the "testfault" boot option, finish by
 * touching an unmapped address, which must end in a page-fault panic;
 * the test harness checks for that. "testapfault" does the same on
 * processor 1, to check that a panic on a secondary processor stops the
 * system.
 */
int
md_selftest(void)
{
        int failures = 0;

        if (curcpu() != &cpu_info_primary) {
                printf("selftest: curcpu() is %p, expected %p\n",
                    (void *)curcpu(), (void *)&cpu_info_primary);
                failures++;
        }

        /* int3 is a trap: the saved rip is already past it. */
        trap_expect(BPTFLT, 0);
        breakpoint();
        if (!trap_expect_done(NULL)) {
                printf("selftest: breakpoint trap not seen\n");
                failures++;
        }

        /* ud2 is a fault: the saved rip points at the 2-byte instruction. */
        trap_expect(INVOPFLT, 2);
        cpu_ud2();
        if (!trap_expect_done(NULL)) {
                printf("selftest: invalid opcode fault not seen\n");
                failures++;
        }

        failures += md_selftest_hat();

        if (boot_option("testapfault") && ncpus > 1) {
                printf("selftest: faulting on cpu1; a page fault panic "
                    "should follow\n");
                smp_call_all(md_fault_on_cpu1, NULL);
                printf("selftest: no fault was raised\n");
                failures++;
        }

        if (boot_option("testfault")) {
                printf("selftest: touching %#jx; a page fault panic "
                    "should follow\n", (uintmax_t)UNMAPPED_VA);
                *(volatile uint64_t *)(uintptr_t)UNMAPPED_VA = 1;
                printf("selftest: no fault was raised\n");
                failures++;
        }

        return failures;
}

/*
 * Every processor now runs on the kernel's page tables, descriptor
 * tables, and stacks, and bootinfo holds copies of everything it needs,
 * so the boot loader's memory can be reused.
 */
void
bootloader_reclaim(void)
{
        size_t pages;

        limine_boot_release();
        pages = page_release_deferred();
        printf("reclaimed %ju KB of boot loader memory\n",
            (uintmax_t)(ptob(pages) / 1024));
}

int
boot_option(const char *word)
{
        const char *p = bootinfo.bi_cmdline;
        size_t len = strlen(word);

        if (len == 0)
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
 * Stop the system. Rebooting is not implemented yet, so every function
 * halts.
 */
void
mdboot(int fcn, int status)
{
        static volatile uint32_t reported;

        disable_intr();
        cpu_halt_others();
        if (atomic_swap_32(&reported, 1) != 0)
                cpu_halt_self_forever();

        if (fcn != AD_HALT)
                printf("mdboot: reboot not implemented, halting\n");

        printf("halted\n");
        cnflush();

        if (boot_option("debugexit"))
                outb(DEBUG_EXIT_PORT,
                    status == 0 ? DEBUG_EXIT_PASS : DEBUG_EXIT_FAIL);

        for (;;) {
                disable_intr();
                cpu_halt();
        }
}
