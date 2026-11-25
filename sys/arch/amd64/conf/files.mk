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
KERNEL_MAP      := sys/arch/amd64/conf/kernel.map
KERNEL_BEGIN_SRC := sys/arch/amd64/amd64/kernel_begin.S
KERNEL_END_SRC  := sys/arch/amd64/amd64/kernel_end.S

# uACPI, the AML interpreter (sys/external/uacpi/README.belka).
UACPI_SRCS      := $(wildcard sys/external/uacpi/source/*.c)

MI_SRCS         := sys/kern/init_main.c \
                   sys/kern/kern_clock.c \
                   sys/kern/kern_cons.c \
                   sys/kern/kern_cred.c \
                   sys/kern/kern_kmem.c \
                   sys/kern/kern_descrip.c \
                   sys/kern/kern_exec.c \
                   sys/kern/kern_exit.c \
                   sys/kern/kern_fork.c \
                   sys/kern/kern_sproc.c \
                   sys/kern/kern_itimer.c \
                   sys/kern/kern_kthread.c \
                   sys/kern/kern_lwp.c \
                   sys/kern/kern_mrlock.c \
                   sys/kern/kern_mutex.c \
                   sys/kern/kern_sched.c \
                   sys/kern/kern_sig.c \
                   sys/kern/kern_time.c \
                   sys/kern/kern_proc.c \
                   sys/kern/kern_resource.c \
                   sys/kern/kern_spinlock.c \
                   sys/kern/kern_syscall.c \
                   sys/kern/kern_synch.c \
                   sys/kern/kern_random.c sys/kern/subr_siphash.c \
                   sys/kern/kern_timeout.c \
                   sys/kern/kern_uadmin.c \
                   sys/kern/sched_rt.c \
                   sys/kern/sched_ts.c \
                   sys/kern/subr_blkdev.c \
                   sys/kern/subr_gpt.c \
                   sys/kern/sys_generic.c \
                   sys/kern/sys_poll.c \
                   sys/kern/tty.c \
                   sys/kern/tty_pty.c \
                   sys/kern/strhead.c \
                   sys/kern/str_conf.c \
                   sys/kern/str_loop.c \
                   sys/kern/str_mods.c \
                   sys/kern/str_mux.c \
                   sys/kern/str_msg.c \
                   sys/kern/str_queue.c \
                   sys/kern/sockfs.c \
                   sys/kern/sock_tpi.c \
                   sys/kern/spec_vnops.c \
                   sys/kern/vfs_bio.c \
                   sys/kern/vfs_dnlc.c \
                   sys/kern/vfs_lookup.c \
                   sys/kern/vfs_mount.c \
                   sys/kern/vfs_reclock.c \
                   sys/kern/vfs_syscalls.c \
                   sys/kern/vfs_vnode.c \
                   sys/kern/vfs_vnops.c \
                   sys/fs/ext2fs/ext2fs_alloc.c \
                   sys/fs/ext2fs/ext2fs_balloc.c \
                   sys/fs/ext2fs/ext2fs_lookup.c \
                   sys/fs/ext2fs/ext2fs_pages.c \
                   sys/fs/ext2fs/ext2fs_subr.c \
                   sys/fs/ext2fs/ext2fs_vfsops.c \
                   sys/fs/ext2fs/ext2fs_vnops.c \
                   sys/fs/fifofs/fifo_vnops.c \
                   sys/fs/tmpfs/tmpfs_vfsops.c \
                   sys/fs/devfs/devfs.c \
                   sys/fs/procfs/procfs.c \
                   sys/fs/tmpfs/tmpfs_vnops.c \
                   sys/kern/subr_prf.c \
                   sys/kern/subr_selftest.c \
                   sys/kern/subr_selftest_sched.c \
                   sys/kern/subr_selftest_dev.c \
                   sys/kern/subr_selftest_vfs.c \
                   sys/kern/subr_selftest_ext2.c \
                   sys/kern/subr_selftest_proc.c \
                   sys/kern/subr_selftest_str.c \
                   sys/libkern/string.c \
                   sys/libkern/crc32.c \
                   sys/vm/vm_kva.c \
                   sys/vm/vm_page.c \
                   sys/vm/vm_pvn.c \
                   sys/vm/vm_anon.c \
                   sys/vm/vm_as.c \
                   sys/vm/vm_segvn.c \
                   sys/vm/vm_mman.c \
                   sys/dev/acpi/acpi.c \
                   sys/dev/acpi/acpi_namespace.c \
                   sys/dev/acpi/acpi_uacpi.c \
                   $(UACPI_SRCS) \
                   sys/dev/pci/pci.c \
                   sys/dev/pci/e1000g.c \
                   sys/dev/pci/nvme.c \
                   sys/dev/pci/ahci.c \
                   sys/dev/virtio/virtio.c \
                   sys/dev/virtio/virtio_pci.c \
                   sys/dev/virtio/vioblk.c \
                   sys/dev/virtio/vionet.c \
                   sys/net/dlether.c \
                   sys/netinet/arp.c \
                   sys/netinet/ip.c \
                   sys/netinet/ip_reass.c \
                   sys/netinet/tcp.c \
                   sys/netinet/tpi_opt.c \
                   sys/netinet/udp.c \
                   sys/dev/ic/ns16550.c

MD_SRCS         := sys/arch/amd64/amd64/locore.S \
                   sys/arch/amd64/amd64/atomic.S \
                   sys/arch/amd64/amd64/cpufunc.S \
                   sys/arch/amd64/amd64/vector.S \
                   sys/arch/amd64/amd64/fpusubr.S \
                   sys/arch/amd64/amd64/copy.S \
                   sys/arch/amd64/amd64/cpu_tables.c \
                   sys/arch/amd64/amd64/trap.c \
                   sys/arch/amd64/amd64/hat.c \
                   sys/arch/amd64/amd64/fpu.c \
                   sys/arch/amd64/amd64/vm_machdep.c \
                   sys/arch/amd64/amd64/sig_machdep.c \
                   sys/arch/amd64/amd64/intr.c \
                   sys/arch/amd64/amd64/ioapic.c \
                   sys/arch/amd64/amd64/acpi_machdep.c \
                   sys/arch/amd64/amd64/pci_machdep.c \
                   sys/arch/amd64/amd64/lapic.c \
                   sys/arch/amd64/amd64/limine_boot.c \
                   sys/arch/amd64/amd64/machdep.c \
                   sys/arch/amd64/amd64/mp_machdep.c
