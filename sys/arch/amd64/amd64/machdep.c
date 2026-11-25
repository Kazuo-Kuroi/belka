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

#include <machine/acpi_machdep.h>
#include <machine/ioapic.h>
#include <machine/atomic.h>
#include <machine/bootinfo.h>
#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/fpu.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <machine/trap.h>
#include <machine/vmparam.h>

#include <sys/intr.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/synch.h>
#include <sys/time.h>

#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>

#include "dev/acpi/acpivar.h"
#include "dev/ic/ns16550var.h"
#include "dev/pci/pcivar.h"

#define CONSOLE_IOBASE  0x3f8   /* COM1 */
#define CONSOLE_SPEED   115200
#define CONSOLE_IRQ     4       /* COM1 */

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

/*
 * Interrupt stacks for the bootstrap processor until md_stacks_init()
 * replaces them with guarded ones.
 */
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
static void md_overflow_on_cpu1(void *);
static int md_expect_read_fault(uint64_t, uint64_t);
static int md_expect_write_fault(uint64_t, uint64_t, uint64_t);
static void md_kva_read_all(void *);
static void md_kva_fault_all(void *);
static int md_selftest_hat(void);
static int md_selftest_intr(void);
static int md_selftest_acpi(void);
static int md_selftest_rtc(void);
static uint8_t rtc_read(uint8_t);
static int64_t rtc_time_of_day(void);
static void rtc_write(uint8_t, uint8_t);
static int rtc_intr(void *);
static int md_rtc_run(int);
static int md_recurse(int);
static void md_note_ipi(void *);
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

void
md_stacks_init(void)
{
        for (int i = 1; i <= NIST; i++)
                cpu_set_ist(&cpu_info_primary, i, kstack_alloc(KM_SLEEP));
        /* With memory allocation up: user floating-point state. */
        fpu_init();
}

static volatile int md_recurse_limit = 1 << 30;

/* Recurse until the stack overflows into its guard page. */
static int
md_recurse(int depth)
{
        volatile char pad[256];

        pad[0] = (char)depth;
        if (depth > md_recurse_limit)
                return 0;
        return md_recurse(depth + 1) + pad[0];
}

static void
md_overflow_on_cpu1(void *arg __unused)
{
        if (cpu_number() == 1)
                (void)md_recurse(0);
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
        HCHECK(hat_kextract(KVA_END - PAGESIZE, &pa) != 0);

        /* KVA mappings, and their removal from every processor's TLB. */
        if ((pp = page_alloc(0, PA_ZERO)) == NULL) {
                HCHECK(pp != NULL);
                return failures;
        }
        va = kva_alloc(1, KM_SLEEP);
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
        kva_free(va, 1);

        page_free(pp, 0);
        return failures;
#undef HCHECK
}

static volatile uint32_t md_ipi_seen[MAXCPUS];

static void
md_note_ipi(void *arg __unused)
{
        md_ipi_seen[cpu_number()]++;
}

/*
 * Every processor ticks, and each can be sent an action IPI on its own.
 */
static int
md_selftest_intr(void)
{
        uint64_t before[MAXCPUS];
        int failures = 0;
        unsigned int n = ncpus;

        for (unsigned int i = 0; i < n; i++)
                before[i] = cpu_info_store[i].ci_ticks;
        drv_usecwait(100000);
        for (unsigned int i = 0; i < n; i++) {
                uint64_t ticks = cpu_info_store[i].ci_ticks - before[i];

                if (ticks < 5) {
                        printf("selftest: cpu%u ticked %ju times in "
                            "100 ms\n", i, (uintmax_t)ticks);
                        failures++;
                }
        }

        for (unsigned int i = 1; i < n; i++) {
                uint32_t seen = md_ipi_seen[i];
                unsigned int waited;

                cpu_action(&cpu_info_store[i], md_note_ipi, NULL);
                for (waited = 0; waited < 100000 && md_ipi_seen[i] == seen;
                    waited += 10)
                        drv_usecwait(10);
                if (md_ipi_seen[i] != seen + 1) {
                        printf("selftest: cpu%u did not run its action\n", i);
                        failures++;
                }
        }
        return failures;
}

/* The MADT must agree with the processors that actually started. */
static int
md_selftest_acpi(void)
{
        int failures = 0;

        if (!madt.mt_valid) {
                printf("selftest: no MADT\n");
                return 1;
        }
        if (madt.mt_nlapic != bootinfo.bi_ncpu) {
                printf("selftest: MADT lists %u processors, boot loader %ju\n",
                    madt.mt_nlapic, (uintmax_t)bootinfo.bi_ncpu);
                failures++;
        }
        for (unsigned int i = 0; i < ncpus; i++) {
                uint32_t id = cpu_info_store[i].ci_lapic_id;
                int found = 0;

                for (unsigned int j = 0; j < madt.mt_nlapic && j < MAXCPUS;
                    j++)
                        found |= madt.mt_lapic_id[j] == id;
                if (!found) {
                        printf("selftest: cpu%u LAPIC id %u not in MADT\n",
                            i, id);
                        failures++;
                }
        }
        if (madt.mt_nioapic == 0 || ioapic_npins() < ISA_NIRQ) {
                printf("selftest: %u I/O APICs with %u pins\n",
                    madt.mt_nioapic, ioapic_npins());
                failures++;
        }
        return failures;
}

/*
 * The CMOS real-time clock's periodic interrupt, ISA IRQ 8, is the test
 * device: it needs no input and is on every PC-compatible machine.
 * Register A selects the rate, bit 6 of register B enables the periodic
 * interrupt, and reading register C acknowledges it.
 */
#define RTC_INDEX       0x70
#define RTC_DATA        0x71
#define RTC_REG_A       0x0a
#define RTC_REG_B       0x0b
#define RTC_REG_C       0x0c
#define RTC_B_PIE       0x40
#define RTC_RATE_64HZ   0x0a
#define RTC_IRQ         8

static struct spinlock rtc_lock = SPINLOCK_INITIALIZER("rtc");
static struct mutex rtc_test_mutex;
static volatile uint32_t rtc_count;
static volatile uint32_t rtc_wrong_context;
static volatile int rtc_expect_thread;

static uint8_t
rtc_read(uint8_t reg)
{
        outb(RTC_INDEX, reg);
        return inb(RTC_DATA);
}

static void
rtc_write(uint8_t reg, uint8_t value)
{
        outb(RTC_INDEX, reg);
        outb(RTC_DATA, value);
}

/*
 * Acknowledge and count one RTC interrupt, checking that the handler
 * runs where it was asked to: in an interrupt thread, able to take a
 * mutex, or in the hardware interrupt itself.
 */
static int
rtc_intr(void *arg __unused)
{
        int s = spin_lock(&rtc_lock);
        uint8_t c = rtc_read(RTC_REG_C);
        int in_hw = curcpu()->ci_intr_depth != 0;

        spin_unlock(&rtc_lock, s);
        if ((c & 0x40) == 0)
                return 0;               /* not a periodic interrupt */
        if (rtc_expect_thread) {
                if (in_hw || (curthread->kt_flags & KTF_ITHREAD) == 0)
                        rtc_wrong_context++;
                else {
                        mutex_lock(&rtc_test_mutex);
                        mutex_unlock(&rtc_test_mutex);
                }
        } else if (!in_hw) {
                rtc_wrong_context++;
        }
        rtc_count++;
        return 1;
}

/* Run the RTC at 64 Hz for half a second in the given mode. */
static int
md_rtc_run(int fast)
{
        void *cookie;
        uint32_t got;
        int s, failures = 0;

        rtc_count = 0;
        rtc_wrong_context = 0;
        rtc_expect_thread = !fast;
        cookie = intr_establish(RTC_IRQ, INTR_ISA | (fast ? INTR_FAST : 0),
            rtc_intr, NULL, "rtc");
        if (cookie == NULL) {
                printf("selftest: cannot attach the RTC interrupt\n");
                return 1;
        }

        s = spin_lock(&rtc_lock);
        rtc_write(RTC_REG_A, (uint8_t)((rtc_read(RTC_REG_A) & 0xf0) |
            RTC_RATE_64HZ));
        rtc_write(RTC_REG_B, (uint8_t)(rtc_read(RTC_REG_B) | RTC_B_PIE));
        (void)rtc_read(RTC_REG_C);
        spin_unlock(&rtc_lock, s);

        delay(HZ / 2);

        s = spin_lock(&rtc_lock);
        rtc_write(RTC_REG_B, (uint8_t)(rtc_read(RTC_REG_B) & ~RTC_B_PIE));
        (void)rtc_read(RTC_REG_C);
        spin_unlock(&rtc_lock, s);
        got = rtc_count;
        intr_disestablish(cookie);

        printf("selftest: RTC %s: %u interrupts in 500 ms\n",
            fast ? "fast handler" : "interrupt thread", got);
        if (got < 20 || got > 45) {
                printf("selftest: expected about 32 RTC interrupts\n");
                failures++;
        }
        if (rtc_wrong_context != 0) {
                printf("selftest: RTC handler ran in the wrong context "
                    "%u times\n", rtc_wrong_context);
                failures++;
        }
        return failures;
}

static int
md_selftest_rtc(void)
{
        int failures;

        mutex_init(&rtc_test_mutex, "rtc_test");
        failures = md_rtc_run(0);
        failures += md_rtc_run(1);
        return failures;
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
        failures += md_selftest_intr();
        failures += md_selftest_acpi();
        failures += md_selftest_rtc();

        if (boot_option("testreboot")) {
                printf("selftest: rebooting through ACPI\n");
                mdboot(AD_BOOT, 0);
        }

        if (boot_option("teststack") && ncpus > 1) {
                printf("selftest: overflowing cpu1's stack; a double "
                    "fault panic should follow\n");
                smp_call_all(md_overflow_on_cpu1, NULL);
                printf("selftest: no fault was raised\n");
                failures++;
        }

        if (boot_option("testuaf")) {
                char *p = kmem_alloc(64, KM_SLEEP);

                printf("selftest: writing to freed memory; a panic "
                    "should follow\n");
                kmem_free(p, 64);
                p[20] = 1;
                p = kmem_alloc(64, KM_SLEEP);
                printf("selftest: write after free not detected\n");
                kmem_free(p, 64);
                failures++;
        }

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
 * Read the CMOS real-time clock once: seconds since 1970, or 0 if it
 * reads as nonsense. Waits out an update in progress and reads until
 * two passes agree. The century is assumed to be 20.
 */
static int64_t
rtc_time_of_day(void)
{
        static const int regs[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
        int v[6], w[6], binary, h24;
        int64_t y, m, d, era, yoe, doy, doe, days;

        for (int tries = 0; tries < 10; tries++) {
                int s = spin_lock(&rtc_lock);

                for (int spin = 0; spin < 100000 &&
                    (rtc_read(RTC_REG_A) & 0x80) != 0; spin++)
                        cpu_spinwait();
                for (int i = 0; i < 6; i++)
                        v[i] = rtc_read((uint8_t)regs[i]);
                for (int i = 0; i < 6; i++)
                        w[i] = rtc_read((uint8_t)regs[i]);
                binary = (rtc_read(RTC_REG_B) & 0x04) != 0;
                h24 = (rtc_read(RTC_REG_B) & 0x02) != 0;
                spin_unlock(&rtc_lock, s);
                if (memcmp(v, w, sizeof(v)) == 0)
                        break;
        }
        if (!binary) {
                for (int i = 0; i < 6; i++) {
                        int pm = i == 2 ? v[i] & 0x80 : 0;

                        v[i] = ((v[i] & 0x7f) >> 4) * 10 + (v[i] & 0x0f);
                        v[i] |= pm;
                }
        }
        if (!h24 && (v[2] & 0x80) != 0)
                v[2] = ((v[2] & 0x7f) % 12) + 12;
        else if (!h24)
                v[2] = (v[2] & 0x7f) % 12;
        if (v[0] > 59 || v[1] > 59 || v[2] > 23 || v[3] < 1 || v[3] > 31 ||
            v[4] < 1 || v[4] > 12)
                return 0;

        /* Days from the civil date (proleptic Gregorian). */
        y = 2000 + v[5];
        m = v[4];
        d = v[3];
        y -= m <= 2;
        era = y / 400;
        yoe = y - era * 400;
        doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        days = era * 146097 + doe - 719468;
        return days * 86400 + v[2] * 3600 + v[1] * 60 + v[0];
}

void
configure(void)
{
        boottime_sec = rtc_time_of_day();
        /* The namespace first: the PCI drivers need its interrupt routes. */
        acpi_namespace_init();
        pci_init();
        /* Console input, for the console terminal. */
        (void)ns16550_intr_attach(&console_softc, CONSOLE_IRQ, cons_input,
            cons_disconnect);
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
 * The value of a "name=value" word in the command line, copied to buf
 * (at most len - 1 bytes, NUL-terminated). Returns 1 if there is one.
 */
int
boot_option_value(const char *name, char *buf, size_t len)
{
        const char *p = bootinfo.bi_cmdline;
        size_t nlen = strlen(name), n;

        if (nlen == 0 || len == 0)
                return 0;
        while (*p != '\0') {
                while (*p == ' ')
                        p++;
                if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
                        p += nlen + 1;
                        for (n = 0; p[n] != ' ' && p[n] != '\0' &&
                            n + 1 < len; n++)
                                buf[n] = p[n];
                        buf[n] = '\0';
                        return 1;
                }
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
        int off = 0;

        /* The firmware hears of a power-off while interrupts are on. */
        if (fcn == AD_POWEROFF && atomic_load_32(&reported) == 0)
                off = acpi_poweroff_prepare() == 0;
        disable_intr();
        cpu_halt_others();
        if (atomic_swap_32(&reported, 1) != 0)
                cpu_halt_self_forever();

        if (fcn == AD_BOOT || fcn == AD_IBOOT) {
                printf("rebooting\n");
                cnflush();
                acpi_reset();
                /* The keyboard controller's reset line, then give up. */
                outb(0x64, 0xfe);
                drv_usecwait(100000);
                printf("mdboot: reset failed, halting\n");
        }

        if (off) {
                printf("powering off\n");
                cnflush();
                acpi_poweroff();
        }
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
