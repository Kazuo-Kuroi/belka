/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Local APIC driver, the legacy 8259 mask, and clock calibration.
 *
 * Mode: x2APIC if the bootstrap processor has it, else xAPIC. In xAPIC
 * mode every processor's registers are at the same physical address,
 * mapped uncached at KVA_LAPIC; each processor sees its own.
 *
 * Calibration: the 8254 PIT, channel 2, times a fixed interval during
 * which the TSC and the local APIC timer are sampled. Every processor
 * is assumed to share both frequencies, which holds for the systems
 * Belka targets. HPET or CPUID frequency leaves can replace the PIT on
 * hardware without one.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/param.h>
#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/intr.h>
#include <machine/intrdefs.h>
#include <machine/lapic.h>
#include <machine/vmparam.h>

#include <vm/hat.h>

/* The 8259 interrupt controllers. */
#define IO_ICU1         0x20
#define IO_ICU2         0xa0

/* The 8254 timer, channel 2, and its gate in the system control port. */
#define PIT_FREQ        1193182
#define PIT_CH2         0x42
#define PIT_MODE        0x43
#define PIT_CTRL        0x61
#define PIT_CTRL_GATE2  0x01
#define PIT_CTRL_SPKR   0x02
#define PIT_CTRL_OUT2   0x20
#define PIT_SEL2_MODE0  0xb0    /* channel 2, lobyte/hibyte, mode 0 */

#define CAL_MS          10      /* length of one calibration interval */
#define CAL_RUNS        5
#define CAL_SPIN_LIMIT  UINT64_C(100000000)

#define ICR_SPIN_LIMIT  1000000

uint64_t tsc_freq;

static int x2apic;
static volatile uint32_t *lapic_mmio;
static int lapic_up;
static uint32_t lapic_timer_count;      /* initial count for one tick */

static uint32_t lapic_read(unsigned int);
static void lapic_write(unsigned int, uint32_t);
static void lapic_send_icr(uint32_t, uint32_t);
static void icu_mask(void);
static int pit_interval(uint64_t *, uint32_t *);

static uint32_t
lapic_read(unsigned int reg)
{
        if (x2apic)
                return (uint32_t)rdmsr(MSR_X2APIC_BASE + (reg >> 4));
        return lapic_mmio[reg / 4];
}

static void
lapic_write(unsigned int reg, uint32_t value)
{
        if (x2apic)
                wrmsr(MSR_X2APIC_BASE + (reg >> 4), value);
        else
                lapic_mmio[reg / 4] = value;
}

/*
 * Reprogram both 8259s to vectors ICU_OFFSET to ICU_OFFSET + 15 and mask
 * every line. Firmware may have left them at vectors that overlap the
 * processor exceptions, and a spurious 8259 interrupt arrives even when
 * all lines are masked; this makes it land on a harmless vector.
 */
static void
icu_mask(void)
{
        outb(IO_ICU1, 0x11);                    /* ICW1: init, ICW4 */
        outb(IO_ICU2, 0x11);
        outb(IO_ICU1 + 1, ICU_OFFSET);          /* ICW2: vector base */
        outb(IO_ICU2 + 1, ICU_OFFSET + 8);
        outb(IO_ICU1 + 1, 0x04);                /* ICW3: slave on IRQ2 */
        outb(IO_ICU2 + 1, 0x02);
        outb(IO_ICU1 + 1, 0x01);                /* ICW4: 8086 mode */
        outb(IO_ICU2 + 1, 0x01);
        outb(IO_ICU1 + 1, 0xff);                /* mask everything */
        outb(IO_ICU2 + 1, 0xff);
}

/*
 * Choose the APIC mode and map the registers. Called once, on the
 * bootstrap processor, after hat_init().
 */
void
lapic_init(void)
{
        uint32_t regs[4];
        uint64_t base;

        icu_mask();

        cpuid(1, 0, regs);
        base = rdmsr(MSR_APIC_BASE);
        if ((regs[2] & CPUID_1_ECX_X2APIC) != 0 ||
            (base & APIC_BASE_X2APIC) != 0) {
                x2apic = 1;
        } else {
                paddr_t pa = base & APIC_BASE_ADDR;

                if (hat_kenter(KVA_LAPIC, pa, HAT_WRITE | HAT_NOCACHE) != 0)
                        panic("lapic_init: cannot map the local APIC");
                lapic_mmio = (volatile uint32_t *)(uintptr_t)KVA_LAPIC;
        }
        lapic_cpu_init();
        lapic_up = 1;
        printf("lapic: %s mode, version %#x\n", x2apic ? "x2APIC" : "xAPIC",
            lapic_read(LAPIC_VERSION) & 0xff);
}

/*
 * Enable the calling processor's local APIC: the chosen mode, the
 * spurious vector, the error vector, LINT1 as NMI on the bootstrap
 * processor, and everything else masked. The timer is started
 * separately, after calibration.
 */
void
lapic_cpu_init(void)
{
        uint64_t base = rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE;

        if (x2apic)
                base |= APIC_BASE_X2APIC;
        wrmsr(MSR_APIC_BASE, base);

        lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
        lapic_write(LAPIC_TPR, 0);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR);
        lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
        if (curcpu() == &cpu_info_primary)
                lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_NMI);
        else
                lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_LVT_ERROR, LAPIC_ERROR_VECTOR);
        lapic_write(LAPIC_ESR, 0);
        lapic_write(LAPIC_ESR, 0);
        lapic_eoi();

        curcpu()->ci_lapic_id = lapic_id();
}

uint32_t
lapic_id(void)
{
        if (x2apic)
                return lapic_read(LAPIC_ID);
        return lapic_read(LAPIC_ID) >> 24;
}

void
lapic_eoi(void)
{
        lapic_write(LAPIC_EOI, 0);
}

int
lapic_ready(void)
{
        return lapic_up;
}

int
lapic_is_x2apic(void)
{
        return x2apic;
}

/* Send the interrupt cmd (vector and delivery bits) to one processor. */
static void
lapic_send_icr(uint32_t dest, uint32_t cmd)
{
        if (x2apic) {
                wrmsr(MSR_X2APIC_BASE + (LAPIC_ICR_LO >> 4),
                    ((uint64_t)dest << 32) | cmd);
                return;
        }
        for (int spin = 0; spin < ICR_SPIN_LIMIT; spin++) {
                if ((lapic_read(LAPIC_ICR_LO) & LAPIC_ICR_PENDING) == 0)
                        break;
                cpu_spinwait();
        }
        lapic_write(LAPIC_ICR_HI, dest << 24);
        lapic_write(LAPIC_ICR_LO, cmd);
}

void
lapic_send_ipi(uint32_t dest, int vector)
{
        lapic_send_icr(dest, LAPIC_ICR_FIXED | LAPIC_ICR_ASSERT |
            (uint32_t)vector);
}

/* Send an NMI to every processor except the caller. */
void
lapic_send_nmi_others(void)
{
        if (!lapic_up)
                return;
        lapic_send_icr(0, LAPIC_ICR_NMI | LAPIC_ICR_ASSERT |
            LAPIC_ICR_ALLEXCL);
}

/*
 * Time one PIT interval of CAL_MS milliseconds, counting TSC ticks and
 * local APIC timer ticks across it. Returns 0, or -1 if the PIT never
 * finishes.
 */
static int
pit_interval(uint64_t *tsc_ticks, uint32_t *lapic_ticks)
{
        uint32_t latch = PIT_FREQ * CAL_MS / 1000;
        uint64_t tsc0, tsc1, spins = 0;
        uint32_t ccr;
        uint8_t ctrl;

        ctrl = (uint8_t)((inb(PIT_CTRL) & ~PIT_CTRL_SPKR) | PIT_CTRL_GATE2);
        outb(PIT_CTRL, ctrl);
        outb(PIT_MODE, PIT_SEL2_MODE0);

        lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR);

        outb(PIT_CH2, (uint8_t)(latch & 0xff));
        outb(PIT_CH2, (uint8_t)(latch >> 8));
        /* Counting starts once the high byte is written. */
        lapic_write(LAPIC_TIMER_ICR, UINT32_MAX);
        tsc0 = rdtsc();
        while ((inb(PIT_CTRL) & PIT_CTRL_OUT2) == 0) {
                if (++spins == CAL_SPIN_LIMIT)
                        return -1;
        }
        tsc1 = rdtsc();
        ccr = lapic_read(LAPIC_TIMER_CCR);
        lapic_write(LAPIC_TIMER_ICR, 0);

        *tsc_ticks = tsc1 - tsc0;
        *lapic_ticks = UINT32_MAX - ccr;
        return 0;
}

/*
 * Measure the TSC and local APIC timer frequencies against the PIT.
 * Several intervals are timed and the median of each is kept, which
 * discards an interval stretched by a host scheduling delay.
 */
void
clock_calibrate(void)
{
        uint64_t tsc[CAL_RUNS];
        uint32_t lap[CAL_RUNS];
        uint32_t regs[4];
        int s = splhi();

        for (int i = 0; i < CAL_RUNS; i++) {
                if (pit_interval(&tsc[i], &lap[i]) != 0)
                        panic("clock_calibrate: the PIT does not count");
        }
        splx(s);

        /* Insertion sort; CAL_RUNS is tiny. */
        for (int i = 1; i < CAL_RUNS; i++) {
                for (int j = i; j > 0 && tsc[j - 1] > tsc[j]; j--) {
                        uint64_t t = tsc[j];

                        tsc[j] = tsc[j - 1];
                        tsc[j - 1] = t;
                }
                for (int j = i; j > 0 && lap[j - 1] > lap[j]; j--) {
                        uint32_t t = lap[j];

                        lap[j] = lap[j - 1];
                        lap[j - 1] = t;
                }
        }

        tsc_freq = tsc[CAL_RUNS / 2] * (1000 / CAL_MS);
        lapic_timer_count = (uint32_t)((uint64_t)lap[CAL_RUNS / 2] *
            (1000 / CAL_MS) / HZ);
        if (lapic_timer_count == 0)
                panic("clock_calibrate: local APIC timer does not count");

        cpuid(0x80000000, 0, regs);
        if (regs[0] >= 0x80000007)
                cpuid(0x80000007, 0, regs);
        else
                regs[3] = 0;
        printf("clock: TSC %ju.%03ju MHz%s, APIC timer %ju kHz, HZ %d\n",
            (uintmax_t)(tsc_freq / 1000000),
            (uintmax_t)(tsc_freq / 1000 % 1000),
            (regs[3] & CPUID_80000007_EDX_INVTSC) != 0 ? " invariant" : "",
            (uintmax_t)lapic_timer_count * HZ / 1000, HZ);
}

/* Start the calling processor's periodic tick. */
void
lapic_timer_start(void)
{
        lapic_write(LAPIC_TIMER_DCR, LAPIC_DCR_DIV16);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_PERIODIC | LAPIC_TIMER_VECTOR);
        lapic_write(LAPIC_TIMER_ICR, lapic_timer_count);
}

/*
 * Busy-wait for at least usecs microseconds, timed by the TSC. Safe in
 * any context once the clock is calibrated.
 */
void
drv_usecwait(unsigned long usecs)
{
        uint64_t start = rdtsc();
        uint64_t ticks;

        if (tsc_freq == 0)
                panic("drv_usecwait: clock not calibrated");
        ticks = tsc_freq / 1000000 * usecs +
            tsc_freq % 1000000 * usecs / 1000000;
        while (rdtsc() - start < ticks)
                cpu_spinwait();
}
