/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Machine-independent kernel startup.
 */

#include <stdint.h>

#include <sys/buf.h>
#include <sys/cred.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/uadmin.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#include <fs/ext2fs/ext2fs.h>

#include <machine/bootinfo.h>
#include <machine/intr.h>
#include <machine/param.h>

#include <vm/anon.h>
#include <vm/as.h>
#include <vm/hat.h>
#include <vm/kva.h>
#include <vm/page.h>
#include <vm/pvn.h>

static const char version[] = "Belka 0.0-dev";
static const char copyright[] =
    "Copyright (c) 2026 Setsuna Software L.C. All rights reserved.";

/*
 * A normal boot: the root file system is ext2 on the disk the command
 * line names (root=vdN; vd0 if none), devfs goes on /dev, and
 * /sbin/init (or init=path) runs as process 1, in single-user mode if
 * the command line says "single". Process 1 never exits.
 */
static void
start_init(void)
{
        static char init_name[] = "init", single[] = "-s";
        static char path_env[] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
        char dev[16] = "vd0", path[MAXPATHLEN] = "/sbin/init";
        char *argv[] = { init_name, NULL, NULL };
        char *envp[] = { path_env, NULL };
        struct ext2fs_args args = { dev, 0 };
        int code, data, error;
        pid_t pid;

        (void)boot_option_value("root", dev, sizeof(dev));
        (void)boot_option_value("init", path, sizeof(path));
        if (boot_option("single"))
                argv[1] = single;
        if ((error = vfs_mountroot("ext2fs", &args)) != 0)
                panic("cannot mount the root file system from %s: "
                    "error %d", dev, error);
        if ((error = vfs_mount("devfs", "/dev", NULL)) != 0)
                printf("init: cannot mount devfs on /dev: error %d\n",
                    error);
        if ((error = proc_spawn_init(path, argv, envp, &pid)) != 0)
                panic("cannot start %s: error %d", path, error);
        if (pid != 1)
                panic("%s started as process %d, not 1", path, (int)pid);
        (void)proc_wait(pid, &code, &data);
        panic("%s exited (code %d, status %d)", path, code, data);
}

/*
 * Entered from machine-dependent startup on the bootstrap processor,
 * with the console working and boot information collected.
 *
 * Subsystems are started in dependency order. With "selftest" on the
 * command line the kernel then runs its self-tests on a tmpfs root and
 * halts; otherwise it starts init.
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
        configure();

        pvn_sysinit();
        vn_sysinit();
        binit();
        vfs_init();
        anon_init();
        as_init();
        cred_init();
        proc_init();
        if (!boot_option("selftest"))
                start_init();

        if (vfs_mountroot("tmpfs", NULL) != 0)
                panic("cannot mount the root file system");

        failures = selftest_run();
        if (failures != 0)
                panic("%d self-test checks failed", failures);
        printf("self-tests passed\n");

        mdboot(AD_HALT, 0);
}
