/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Descriptor tables: the shared IDT, and each processor's GDT and TSS.
 */

#include <stdint.h>

#include <sys/systm.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>
#include <machine/segments.h>
#include <machine/trap.h>
#include <machine/tss.h>

/* Entry stubs for vectors 0 to 31, defined in vector.S. */
extern const uint64_t trap_vector_table[NEXCEPTIONS];

/* The IDT is shared by all processors; each gate takes two words. */
static uint64_t idt[NIDT * 2];

/*
 * Build the IDT. Exceptions that can arrive on a bad kernel stack
 * switch to a dedicated stack through the IST. The breakpoint gate is
 * reachable from user mode so that int3 can serve debuggers later.
 * Vectors above 31 stay not-present until interrupt handling exists;
 * delivering one raises a general-protection fault.
 */
void
idt_init(void)
{
        for (int v = 0; v < NEXCEPTIONS; v++) {
                int ist = 0;
                int dpl = SEL_KPL;

                switch (v) {
                case DBLFLT:
                        ist = IST_DBLFLT;
                        break;
                case NMIFLT:
                        ist = IST_NMI;
                        break;
                case MCEFLT:
                        ist = IST_MCE;
                        break;
                case BPTFLT:
                        dpl = SEL_UPL;
                        break;
                default:
                        break;
                }
                set_gate_desc(&idt[v * 2], trap_vector_table[v], KCSEL, ist,
                    SDT_SYS386IGT, dpl);
        }
}

/*
 * Give processor ci its own GDT and TSS and load them, load the shared
 * IDT, and point the GS base at ci. rsp0 is the stack for entry from
 * user mode, and ist_top holds the tops of the NIST interrupt stacks.
 *
 * Interrupts must be disabled.
 */
void
cpu_tables_init(struct cpu_info *ci, uint64_t rsp0, const uint64_t *ist_top)
{
        ci->ci_self = ci;

        ci->ci_gdt[GDT_NULL] = 0;
        ci->ci_gdt[GDT_KCODE] = SD_KCODE64;
        ci->ci_gdt[GDT_KDATA] = SD_KDATA;
        ci->ci_gdt[GDT_UCODE32] = SD_UCODE32;
        ci->ci_gdt[GDT_UDATA] = SD_UDATA;
        ci->ci_gdt[GDT_UCODE] = SD_UCODE64;
        ci->ci_gdt[GDT_LDT] = 0;
        ci->ci_gdt[GDT_LDT + 1] = 0;

        memset(ci->ci_tss, 0, sizeof(ci->ci_tss));
        tss_set64(ci->ci_tss, TSS_RSP0, rsp0);
        for (int i = 1; i <= NIST; i++)
                tss_set64(ci->ci_tss, TSS_IST(i), ist_top[i - 1]);
        /* An I/O map base past the limit means no I/O permission map. */
        ci->ci_tss[TSS_IOBASE] = (uint32_t)TSS_SIZE << 16;
        set_sys_desc(&ci->ci_gdt[GDT_TSS], (uint64_t)(uintptr_t)ci->ci_tss,
            TSS_SIZE - 1, SDT_SYS386TSS);

        lgdt_load((uint64_t)(uintptr_t)ci->ci_gdt,
            (uint16_t)(sizeof(ci->ci_gdt) - 1), KCSEL, KDSEL);
        ltr(TSSSEL);
        lidt_load((uint64_t)(uintptr_t)idt, (uint16_t)(sizeof(idt) - 1));

        /*
         * Loading %gs above cleared its base. While in the kernel the
         * GS base points at cpu_info; the user value lives in
         * KERNELGSBASE and is exchanged by swapgs on entry and exit.
         */
        wrmsr(MSR_GSBASE, (uint64_t)(uintptr_t)ci);
        wrmsr(MSR_KERNELGSBASE, 0);
}
