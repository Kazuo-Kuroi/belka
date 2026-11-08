/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-independent kernel startup.
 */

#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/uadmin.h>

#include <machine/intr.h>
#include <machine/param.h>

#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>

static const char version[] = "Belka 0.0-dev";
static const char copyright[] =
    "Copyright (c) 2026 Setsuna Software L.C. All rights reserved.";

/*
 * Entered from machine-dependent startup on the bootstrap processor,
 * with the console working and boot information collected.
 *
 * Only the self-tests exist so far; subsystem initialisation is added
 * here in dependency order as each subsystem arrives.
 */
void
main(void)
{
        struct page_stats st;
        int failures;

        printf("\n%s\n%s\n\n", version, copyright);

        page_init();
        page_get_stats(&st);
        printf("pages: %zu managed, %zu free (%ju KB)\n",
            st.ps_managed, st.ps_free,
            (uintmax_t)(ptob(st.ps_free) / 1024));

        hat_init();
        kmem_init();
        kva_init();
        md_stacks_init();
        intr_init();
        sched_init();
        cpu_boot_secondary_processors();
        bootloader_reclaim();
        spl0();

        failures = selftest_run();
        if (failures != 0)
                panic("%d self-test checks failed", failures);
        printf("self-tests passed\n");

        mdboot(AD_HALT, 0);
}
