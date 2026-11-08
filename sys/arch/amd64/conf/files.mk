#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Source lists and machine-dependent flags for the amd64 kernel.
#

MD_CFLAGS       := -mcmodel=kernel -mno-red-zone \
                   -mno-mmx -mno-sse -mno-sse2 -mno-avx
MD_ASFLAGS      :=

LDSCRIPT        := sys/arch/amd64/conf/kernel.ld

MI_SRCS         := sys/kern/init_main.c \
                   sys/kern/kern_clock.c \
                   sys/kern/kern_cons.c \
                   sys/kern/kern_kmem.c \
                   sys/kern/kern_kthread.c \
                   sys/kern/kern_mrlock.c \
                   sys/kern/kern_mutex.c \
                   sys/kern/kern_sched.c \
                   sys/kern/kern_spinlock.c \
                   sys/kern/kern_synch.c \
                   sys/kern/kern_timeout.c \
                   sys/kern/sched_rt.c \
                   sys/kern/sched_ts.c \
                   sys/kern/subr_blkdev.c \
                   sys/kern/vfs_bio.c \
                   sys/kern/vfs_dnlc.c \
                   sys/kern/vfs_lookup.c \
                   sys/kern/vfs_mount.c \
                   sys/kern/vfs_vnode.c \
                   sys/kern/vfs_vnops.c \
                   sys/fs/ext2fs/ext2fs_alloc.c \
                   sys/fs/ext2fs/ext2fs_balloc.c \
                   sys/fs/ext2fs/ext2fs_lookup.c \
                   sys/fs/ext2fs/ext2fs_pages.c \
                   sys/fs/ext2fs/ext2fs_subr.c \
                   sys/fs/ext2fs/ext2fs_vfsops.c \
                   sys/fs/ext2fs/ext2fs_vnops.c \
                   sys/fs/tmpfs/tmpfs_vfsops.c \
                   sys/fs/tmpfs/tmpfs_vnops.c \
                   sys/kern/subr_prf.c \
                   sys/kern/subr_selftest.c \
                   sys/kern/subr_selftest_sched.c \
                   sys/kern/subr_selftest_dev.c \
                   sys/kern/subr_selftest_vfs.c \
                   sys/kern/subr_selftest_ext2.c \
                   sys/libkern/string.c \
                   sys/vm/vm_kva.c \
                   sys/vm/vm_page.c \
                   sys/vm/vm_pvn.c \
                   sys/dev/acpi/acpi.c \
                   sys/dev/pci/pci.c \
                   sys/dev/virtio/virtio.c \
                   sys/dev/virtio/virtio_pci.c \
                   sys/dev/virtio/vioblk.c \
                   sys/dev/ic/ns16550.c

MD_SRCS         := sys/arch/amd64/amd64/locore.S \
                   sys/arch/amd64/amd64/atomic.S \
                   sys/arch/amd64/amd64/cpufunc.S \
                   sys/arch/amd64/amd64/vector.S \
                   sys/arch/amd64/amd64/cpu_tables.c \
                   sys/arch/amd64/amd64/trap.c \
                   sys/arch/amd64/amd64/hat.c \
                   sys/arch/amd64/amd64/intr.c \
                   sys/arch/amd64/amd64/ioapic.c \
                   sys/arch/amd64/amd64/acpi_machdep.c \
                   sys/arch/amd64/amd64/pci_machdep.c \
                   sys/arch/amd64/amd64/lapic.c \
                   sys/arch/amd64/amd64/limine_boot.c \
                   sys/arch/amd64/amd64/machdep.c \
                   sys/arch/amd64/amd64/mp_machdep.c
