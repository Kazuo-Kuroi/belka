/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-independent kernel startup.
 */

#include <sys/systm.h>
#include <sys/uadmin.h>

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
        int failures;

        printf("\n%s\n%s\n\n", version, copyright);

        failures = selftest_run();
        if (failures != 0)
                panic("%d self-test checks failed", failures);
        printf("self-tests passed\n");

        mdboot(AD_HALT, 0);
}
