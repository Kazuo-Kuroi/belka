# Belka Design Decisions

Decisions agreed before the first line of kernel code. Each entry states
the decision and, where it is not obvious, the reason. Later decisions are
appended with a date; a decision that is reversed is struck through and
the replacement is linked, not deleted.

Recorded 2026-11-07.

## Ground rules

- **Clean room.** Names and concepts already public through SVR4 and
  DDI/DKI books, manual pages, and ABI specifications may be reused
  (`vnode`, `VOP_*`, `queue_t`, `mblk_t`, `sv_wait`). Algorithms and data
  layouts are written fresh from behavioural descriptions. Structure
  layouts and constants are copied only where an ABI requires them.
- **Licence.** MIT. Every source file carries a header with the creation
  date, the Setsuna Software L.C. copyright line, and
  `SPDX-License-Identifier: MIT`.
- **Workflow.** One commit per coherent step on `main`. Nothing is pushed
  without an explicit request.

## Language and toolchain

- **C99** is the baseline. C11 features are not used.
- **GCC and Clang** are the primary compilers, but the kernel must not be
  locked to them. Every extension (inline assembly, attributes, builtins)
  lives behind `<sys/compiler.h>` or a machine-dependent header and has a
  portable fallback, usually an out-of-line routine in a `.S` file.
- Atomic operations and memory barriers are provided by
  `<machine/atomic.h>`, not `<stdatomic.h>`.
- Assembly uses AT&T syntax in `.S` files, so it builds with both the GNU
  assembler and Clang's integrated assembler.
- **GNU make** is the build tool.
- **Style:** `CODING_STYLE.md`, with **8-space** indentation (the
  examples in that file are authoritative over its section 8 text).

## Platform

- **x86-64 first.** Other architectures are added only after the x86-64
  port is stable on multiprocessors.
- **Boot:** the Limine boot protocol, under UEFI. BTX was considered and
  rejected: it is BIOS-only, 32-bit, and tied to the FreeBSD loader.
- **Platform layer:** a PSM-style module that owns ACPI, the local APIC
  and x2APIC, the I/O APIC, and HPET and TSC time. It is the seam future
  ports implement.
- **Test target:** QEMU `q35` with OVMF and KVM, at least 4 CPUs. Real
  hardware is deferred. See [qemu-setup.md](../qemu-setup.md).

## Core kernel

- **Synchronisation:** spinlocks that block interrupts, mutexes with
  priority inheritance, synchronisation variables, counting semaphores,
  and multi-reader locks. Lock order is documented as it is introduced,
  and a debug build checks lock assertions.
- **Execution model:** IRIX-style. The kernel is preemptible, device
  interrupts run as threads, and there is one run queue per CPU.
  MIPS-specific assumptions (software-managed TLB, ASID recycling by TLB
  cycling, K0/K1 segments, ll/sc windows) are discarded in favour of
  their x86-64 equivalents (PCID, INVLPG and shootdown IPIs, a direct
  map, locked instructions).
- **Processes:** the SVR4 model of a process holding one or more LWPs.
- **Virtual memory:** the SVR4 split of address space, segment drivers,
  anonymous memory, and a hardware address translation (HAT) layer, with
  IRIX behaviours on top: free pages keep their object identity and can
  be reclaimed by a fault, the page allocator is NUMA-aware, and TLB
  shootdown is lazy where it is safe.
- **VFS:** IRIX-style vnode and VFS layer, a unified page cache, and a
  small buffer cache for metadata.

## Physical memory (2026-11-07)

- **Buddy allocator** per memory node, orders 0 to 10 (4 KB to 4 MB),
  with every block aligned to its own size in physical memory, so the
  same allocator supplies 2 MB large pages. Interface:
  `page_alloc(order, flags)`, `page_alloc_node()`, `page_free()`.
- One `struct page` per managed frame, in a single array taken from the
  end of the largest usable segment. Free blocks never cross a segment
  boundary.
- Physical page 0 is never managed, so a zero physical address is never
  valid.
- Memory the boot loader marks reclaimable is registered as deferred
  segments and released once every processor runs on the kernel's own
  tables and stacks (`page_release_deferred()`).
- **Still to come:** a cache list that keeps freed file and anonymous
  pages' identity (the IRIX behaviour), per-node locks, and nodes from
  ACPI SRAT.

## Multiprocessor start-up and spin locks (2026-11-07)

- **Spin locks** are test-and-test-and-set. Acquiring raises to splhi
  and returns the previous level as a cookie (`s = spin_lock(&l)`,
  `spin_unlock(&l, s)`), and interrupts stay enabled between attempts
  when they were enabled on entry, as in IRIX. Queued locks were
  rejected because they cannot let interrupts in while waiting. Locks
  record their holder; recursion, a foreign release, and a wait of 2^32
  polls panic.
- **Interrupt levels:** `splhi()` and `splx()` only. Device interrupts
  will be threads, so device priorities collapse into splhi.
- **Secondary processors** are started through the Limine MP feature.
  Each gets a 16 KB kernel stack and three interrupt stacks from the page
  allocator, its own GDT and TSS, and a `cpu_info` in
  `cpu_info_store[]`.
- **Panic** stops every other processor and bypasses the console lock.
- Lock order is kept in [lock-order.md](lock-order.md).

## Kernel page tables (2026-11-07)

- The machine-dependent layer is called the **HAT**, after System V
  (`<vm/hat.h>`). Only the kernel address space exists so far.
- **Layout** (`<machine/vmparam.h>`): the lower half is empty in the
  kernel's tables, so stray low addresses fault. The kernel half holds
  the direct map at the boot loader's base (so pointers made before the
  switch stay valid), a 512 GB kernel virtual area for run-time
  mappings, and the kernel image at its link address.
- All 256 kernel-half PML4 entries are populated at boot and never
  change; user address spaces will share them by copying.
- **Only RAM is direct-mapped**, write-back, with 1 GB, 2 MB, or 4 KB
  pages as alignment allows. Holes stay unmapped so device memory is
  never mapped cacheable by accident. Device memory goes in KVA with
  `HAT_NOCACHE` or `HAT_WCOMBINE`.
- **W^X**: kernel text is read-only and executable, read-only data is
  not writable or executable, data and bss are not executable, and the
  direct map is not executable. Every kernel mapping is global.
- The PAT is programmed explicitly on every processor, to the layout
  the Limine protocol documents.
- **TLB shootdown** of KVA mappings goes through `smp_call_all()` until
  IPIs exist; new mappings need no flush.
- Boot loader strings are copied into `bootinfo`, and the RSDP is kept
  as a physical address, so nothing points into loader memory after it
  is reclaimed.

## Interrupts, clock, and IPIs (2026-11-07)

- **Local APIC** in x2APIC mode when the bootstrap processor supports
  it, else xAPIC through an uncached fixed KVA mapping; all processors
  use the same mode. The 8259s are remapped to 0x20 and fully masked.
- **Vectors** (`<machine/intrdefs.h>`): 0x30-0xef for devices later,
  0xf0 timer, 0xf1 action IPI, 0xfe APIC error, 0xff spurious. Vectors
  32-255 have 16-byte stubs that all enter `trap()`.
- **Clock:** the TSC and APIC timer are calibrated against PIT channel
  2 (median of five 10 ms intervals). Every processor runs a periodic
  APIC tick at HZ = 100; the bootstrap processor's also drives the
  machine-independent `clock()` and `lbolt`. `drv_usecwait()` is
  TSC-based. HPET or CPUID frequency leaves will be needed on hardware
  without a PIT.
- **Cross-processor actions** follow IRIX: a per-processor FIFO of
  function calls, posted with `cpu_action()` plus an IPI and run at
  splhi. `smp_call_all()` is built on them. Every waiter serves its own
  queue while it waits, so overlapping and nested calls, even from
  interrupt context, complete.
- **Halting** sets a flag and sends an NMI to every other processor;
  the NMI handler halts when the flag is set.
- **Idle** processors sleep with `sti; hlt` and wake for ticks and
  IPIs. Interrupt handlers run with interrupts disabled and do not nest.
  Device interrupts will be dispatched to interrupt threads.

## Kernel memory allocation (2026-11-08)

- **Interface:** `kmem_alloc()`, `kmem_zalloc()`, and `kmem_free()` with
  the System V DDI contract (the size is passed again on free, flags
  `KM_SLEEP` and `KM_NOSLEEP`), and fixed-size object zones with the
  IRIX `kmem_zone_*` names.
- **Zones** are slab allocators: slabs of 2^order pages from the page
  allocator, in the direct map, header at the end, every page pointing
  at its slab through `p_private`. One empty slab is kept per zone.
- **kmem_alloc** serves 28 size classes up to 4 KB from zones, 16-byte
  aligned; larger requests get whole pages, page aligned.
- **Until the kernel can wait for memory**, `KM_SLEEP` failure panics.
- **Debugging is always on:** per-slab allocation bitmaps catch double
  frees and foreign pointers; free objects are poisoned and checked on
  reuse to catch writes after free.
- **KVA** above the fixed mappings is handed out by `kva_alloc()`, a
  first-fit list of free ranges, to be replaced by a vmem-style arena
  when use grows.
- **Kernel stacks** are 16 KB in KVA with an unmapped guard page below;
  an overflow double faults onto an IST stack and panics. Secondary
  processors start on a one-page direct-map hatch stack, switch to the
  kernel's tables there, then move to their guarded stack. The
  bootstrap processor's own boot stack is still in `.bss` without a
  guard.
- **Not yet:** per-processor object caches (magazines), constructors,
  zone reclaim under memory pressure.

## Threads, scheduling, and sleeping locks (2026-11-08)

- **kthreads** (`<sys/kthread.h>`) are what the scheduler runs; user
  LWPs will be built on them. Each has a guarded 16 KB stack. Exited
  threads are freed by a reaper thread, because a thread cannot unmap
  its own stack and a context switch cannot do a TLB shootdown.
- The boot context becomes **thread0**, bound to processor 0. Each
  processor has an **idle thread**; on secondary processors it is the
  start-up context. Idle processors steal work from the others.
- **Priorities** 0-127, higher more urgent. One run queue per
  processor with a bitmap of non-empty priorities. Equal priorities
  share by a 10-tick slice; a preempted thread returns to the head of
  its priority, a yielding or expired one to the tail (IRIX).
- **Preemptible kernel:** a pending reschedule is taken on the way out
  of any interrupt that arrived at spl0. Wakeups on the local processor
  use a self-IPI so the switch happens as soon as interrupts are on.
- **Context switch locking:** the leaving thread's `kt_lock` is held
  until the processor is off its stack, satisfying the IRIX rule that a
  thread's previous processor must leave its stack before another runs
  it. A dispatcher that finds a better thread whose lock is momentarily
  held re-arms the reschedule rather than waiting for the next slice.
- **Wakeup placement:** the last processor if idle, else any idle
  processor, else the last one. Bound threads always go home, and a
  running thread that binds itself elsewhere migrates at once.
- **Sleeping locks** (`<sys/synch.h>`), with IRIX names: `mutex` with
  ownership handoff and one-level priority inheritance, `sv`
  synchronisation variables, counting `sema` (`psema`/`vsema`), and
  `mrlock` (waiting writers block new readers; a releasing writer admits
  all waiting readers). Sleeping is two-phase so a sleeper never holds
  an object lock and its own thread lock together.
- **Callouts:** System V `timeout()`, `untimeout()`, and `delay()`, on
  one sorted list run from the bootstrap processor's tick.
- **Not yet:** gang, batch, and frame scheduling, transitive priority
  inheritance, per-processor callouts, and timed or interruptible sleeps.

## ACPI, the I/O APIC, and interrupt threads (2026-11-08)

- **ACPI static tables** only (`sys/dev/acpi`): the RSDP is validated,
  the XSDT (or RSDT) walked, and every table with a good checksum
  mapped read-only for good. Fields are read with byte-wise
  little-endian helpers, never structure overlays. There is no AML
  interpreter yet, so PCI interrupt routing (`_PRT`) is not available.
- **MADT** gives the local APICs (cross-checked against the processors
  that start), the I/O APICs, and ISA interrupt overrides; **FADT** gives
  the reset register, which `mdboot(AD_BOOT)` uses before falling back
  to the keyboard controller.
- **I/O APIC:** all pins start masked; each routed GSI gets its own
  vector from 0x30-0xef, delivered to the bootstrap processor in
  physical mode. Spreading interrupts over processors comes later.
- **Interrupt attachment** uses the NetBSD names (`intr_establish`,
  `intr_disestablish`) for the planned driver model. Lines may be
  shared; handlers return whether they claimed the interrupt.
- **Interrupt threads (IRIX):** by default the hardware interrupt masks
  a level-triggered line and wakes the source's thread at `PRI_INTR`
  (100), which runs the handlers and unmasks the line, so handlers can
  take mutexes. `INTR_FAST` runs a handler in the hardware interrupt.
- **Test device:** the CMOS RTC's periodic interrupt (ISA IRQ 8).

## Scheduling classes (2026-11-08)

- **Organisation** after UnixWare: a class operations vector (enter,
  leave, tick, slice expiry) per class, all mapping onto the one
  dispatcher priority scale. **Policies** after IRIX.
- **SCHED_TS**, fair-share time sharing (the IRIX weighted tree): two
  dispatch levels only, `PRI_TS_CREDIT` (2) and `PRI_TS_OVER` (1). Each
  job has a weight from the IRIX table (`w[1] = 1000`,
  `w[i+1] = w[i] + w[i]/10`) and a microsecond balance. Each tick debits
  the running job; once per tick, on a rotating processor, the debits
  are returned to active jobs in proportion to weight, and threads move
  between the levels by the sign of their balance. Credit is capped at
  two slices per thread. Threads joined with `ts_join()` share a job, as
  the threads of a process will.
- **Deviations from a literal reading:** a running thread is not lowered
  by redistribution, only at the end of its slice (IRIX does the same);
  and within a time-share level the run queue orders threads by when
  they last ran, because whole jobs move between levels at once and job
  list order would otherwise starve one of a job's threads.
- **SCHED_RR / SCHED_FIFO:** fixed priorities from `PRI_TS_BOOST` (3) up;
  RR is sliced among equals, FIFO never. Kernel threads default to RR;
  interrupt threads are FIFO at `PRI_INTR`.
- **IRIX lock rules:** only RR/FIFO ("priority scheduled") waiters are
  ordered by priority on sleep queues and cause inheritance; time-share
  waiters queue FIFO behind them. A time-share thread handed a mutex is
  boosted to `PRI_TS_BOOST` until it releases it.
- **Not yet:** a `priocntl`/`schedctl` system call interface, nice
  values from user processes, batch (Miser), gang, and the frame
  scheduler.

## PCI and virtio (2026-11-08)

- **Configuration space** through ECAM from the ACPI MCFG table, each
  bus's megabyte mapped uncached on first use; the legacy 0xcf8/0xcfc
  mechanism if there is no MCFG.
- **Enumeration** walks from bus 0 through PCI-to-PCI bridges, sizes
  BARs with decoding off, and keeps the firmware's assignments.
- **MSI-X** is the interrupt mechanism for PCI devices (no AML, so no
  `_PRT` routing for INTx yet). Each entry gets its own vector and
  interrupt thread through `intr_establish_msi()`, delivered to the
  bootstrap processor.
- **Drivers** match from a static table for now; the names (match,
  attach, softc) are chosen to grow into the planned NetBSD-style
  autoconfiguration.
- **Virtio**: the modern (1.x) PCI interface only, located through the
  vendor capabilities; transitional devices work through it, legacy-only
  ones are refused. Split virtqueues of up to 128 entries, rings in
  page-allocator pages, one MSI-X vector per queue. Buffers are
  translated page by page, so any kernel virtual address works.
- **virtio-blk** (`vioblk`) registers as `vd0`, `vd1`, ... in a minimal
  block device registry (`<sys/blkdev.h>`): synchronous read, write, and
  flush, split at 64 KB. The buffer cache and asynchronous I/O come with
  the VFS.
- **errno** values (`<sys/errno.h>`) are the System V i386 ABI numbers
  as UnixWare uses them.
- **Test disk:** `tools/mkdisk.c` writes an 8 MB image with a
  per-sector pattern; every QEMU run attaches it as a modern-only
  virtio-blk device.

## VFS and tmpfs (2026-11-08)

- **Interfaces** after System V/UnixWare: `struct vnode` with a
  `vnodeops` vector and `VOP_*` macros, `struct vfs` with `vfsops`,
  `vfssw` by name, `uio`/`iovec`, `vattr` with `va_mask`, `cred`, and
  `dirent`. File mode bits and open flags take the System V values.
- **Concurrency contract** after IRIX: the generic layer holds a vnode
  reference across every operation and takes no vnode lock; each file
  system serialises itself. `VOP_INACTIVE` runs with no locks and
  `VINACT` set.
- **Pathname lookup** follows the UnixWare rules (limits 1024/255/20,
  `EINVAL` for the parent of `/` and for a last component that is the
  parent, trailing slashes ignored, `..` at the root stays, `..` at a
  mounted root crosses out, mount points crossed downward, an empty
  link means `.`). Mounting over a file system root is `EBUSY`.
- **Name cache** in the generic layer (IRIX), names up to 31 bytes, no
  negative entries. **Deviation:** entries hold vnode references, the
  classic System V model, because IRIX's soft references depend on its
  global vnode cache; name-space changes purge what they change.
- **Vnodes belong to their file system** for now. The IRIX global vnode
  cache, with free lists and reclaim, comes with the first on-disk file
  system, together with the buffer cache and the unified page cache.
- **tmpfs** is the root file system until ext2: per-mount mutex for the
  name space, per-node mrlock for data, pages from the page allocator
  with holes, a per-mount page limit (`ENOSPC`), and nodes freed at
  their last reference once unlinked.
- **No time-of-day clock yet:** file times count from boot.
- **Kernel file API** (`vn_open`, `vn_rdwr`, `vn_mkdir`, ...) by path;
  system calls will be thin layers over it.

## Buffer cache and ext2 (2026-11-08)

- **Buffer cache** (`<sys/buf.h>`, `kern/vfs_bio.c`) with the System V
  interface: `getblk`, `bread`, `bwrite`, `bdwrite`, `brelse`, plus
  `bflush` and `binval` per device. Buffers are keyed by device and
  512-byte sector, hashed, and kept on one LRU free list; they are
  allocated on demand up to a fixed limit, and a dirty buffer is written
  before it is reused. A `bdflush` thread writes delayed buffers once
  they are 3 seconds old. The cache sits below the file systems only;
  the unified page cache is still to come.
- **ext2** (`sys/fs/ext2fs`) is a fresh MIT implementation of the public
  ext2 revision 1 format. It takes NetBSD's `ext2fs` as its model for
  the file split, function names, and allocation policy (directories
  spread to groups with free space, file blocks near the previous one),
  but no code is copied from it. This replaces the earlier plan to port
  the BSD sources.
- **ext2 features:** `filetype`, `sparse_super`, `large_file`
  read-write; `dir_index` directories are read linearly and lose their
  index flag when changed, which e2fsck accepts and can rebuild;
  `resize_inode` and `ext_attr` are tolerated, and an inode with an
  extended attribute block is never freed. An unknown read-only
  compatible feature, or a file system not cleanly unmounted, mounts
  read-only; an unknown incompatible feature refuses the mount.
- **ext2 locking:** a per-mount mutex covers the name space, block and
  inode allocation, the superblock and group descriptors, and the
  in-core inode table; a per-inode mrlock covers file data. The mrlock
  is taken first.
- **Inodes** live in a per-mount hash table. The IRIX global vnode cache
  is still deferred; a per-mount table is enough until there is memory
  pressure to reclaim under.
- **Time of day** comes from the CMOS real-time clock at boot (assumed
  UTC, century 20xx); `gethrestime()` adds the ticks since. File times
  are now real.
- **Sleep and wakeup, revised:** a dequeued sleeper stays blocked until
  the waker calls `wakeup_thread()`, which clears the wait channel and
  wakes it in one step under the sleeper's thread lock. The waker drops
  the object's lock first, but keeps interrupts blocked until the wakeup.
  This closes a race in which a slow waker could wake a thread out of a
  later, unrelated sleep. It also lets a sleeper free the object it
  slept on as soon as it wakes, such as an I/O request or an on-stack
  semaphore. Sleep queues have their own links, apart from the run
  queues, because a thread that has queued itself can still be
  preempted before it sleeps. `delay()` uses an on-stack callout and
  never allocates.
- **Tests:** two ext2 images (1 KB and 4 KB blocks) are built by
  `mke2fs` before every QEMU boot. The kernel tests them, then leaves
  files behind. After `make test`, the host checks both images with
  `e2fsck -fn` and reads those files back with `debugfs`.

## File systems

1. **tmpfs**, for testing.
2. **ext2**, the first practical file system: a fresh MIT implementation
   modelled on NetBSD's `ext2fs` (see above).
3. **XFS**, specified from the published XFS design documents: on-disk
   format version 4 with version 2 directories. The Linux implementation
   is not followed.

## ABI

- **Native ABI:** AMD64 System V psABI, with system calls entered through
  the `syscall` instruction.
- **System call numbers and errno values** follow UnixWare where
  practical.
- **Space is reserved** for a later UnixWare i386 ABI personality: its
  call gate, numbering, and structure layouts must remain implementable
  without renumbering the native table.

## Drivers

- **First driver interface:** deliberately simple, statically linked,
  and only for bring-up and testing.
- **Long term:** a simplified NetBSD-style autoconfiguration and driver
  model (`autoconf`, `bus_space`, `bus_dma`), so that drivers can be
  imported from the NetBSD tree with little change.
- **Initial set:** 16550 serial and framebuffer consoles; virtio block,
  network, and console; then AHCI, NVMe, e1000, and xHCI.

## STREAMS and networking

- **Classic SVR4 STREAMS** with multiprocessor synchronisation. The
  "next-generation STREAMS" redesign is deferred.
- **Network stack:** the UnixWare architecture (TCP/IP as STREAMS
  modules over DLPI drivers).
- **Socket interface** (decided 2026-11-07): a kernel socket layer that
  translates socket calls into TPI messages to the STREAMS transport
  providers, in the manner of later Solaris `sockfs`. UnixWare 2's
  user-library emulation over TLI and `sockmod` is not used. mlibc sees
  ordinary socket system calls.

## Userland and scope

- **C library:** mlibc, with a Belka system-dependent layer.
- **Out of scope for now:** MAC, ACLs, auditing, and other security
  features. Their hooks are not designed in until they are scheduled.

## Open questions

- **NetBSD network drivers under DLPI.** Imported drivers expect `ifnet`.
  A shim from `ifnet` to DLPI is needed whichever socket interface is
  chosen.
