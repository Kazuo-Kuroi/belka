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
- **Linker** (decided 2026-11-09): the illumos link-editor (`ld` and
  `libld`, from the SVR4 lineage UnixWare's linker shares) links the
  kernel and every user program, whichever compiler built them; see The
  link-editor below. GNU ld remains available (`LINKER=gnu`); lld is
  not used. It also links mlibc's shared `ld.so` (the runtime linker)
  and `libc.so`; see Shared libraries below.
- **Other licences:** code under a licence other than MIT, such as
  illumos's CDDL, may be used. It stays in its own clearly marked place
  (a separate repository like `contrib/mlibc`, or a directory named for
  the licence), keeps its files' licence and notices, and is never mixed
  into MIT files.
- **Python** is not used in the build (decided 2026-11-09). mlibc's
  meson build runs under muon and samurai, C implementations of meson
  and ninja.
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
  start-up context. Idle processors steal work from the others, and so
  does a busy one whose thread's time slice ends with nothing as good
  queued locally, taking a thread of at least that priority (without
  that, a spinning thread could keep one processor while equal or better
  threads waited behind another's work indefinitely).
- **Priorities** 0-127, higher more urgent. One run queue per
  processor with a bitmap of non-empty priorities. Equal priorities
  share by a 10-tick slice; a preempted thread returns to the head of
  its priority, a yielding or expired one to the tail (IRIX). A
  preempted thread keeps the rest of its slice: given a new one, a
  thread interrupted more often than once a slice never reached a slice
  end, so its class never lowered it and it kept the processor (fixed
  2026-11-13; the self-test now runs the fair-share cases with a
  7-tick preemptor).
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
  their last turn ended, because whole jobs move between levels at once
  and job list order would otherwise starve one of a job's threads. A
  turn ends when a thread sleeps, yields, or uses up its slice, not when
  it is preempted, and the time is taken before it is queued again;
  stamped after, an expired thread was sorted by its previous turn and
  could run again ahead of the other threads of its job.
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

## Vnode cache and page cache (2026-11-08)

- **Global vnode cache** (IRIX): shared vnodes come from `vn_alloc()`
  and are never freed, so a pointer to one always points at a vnode, and
  `v_number` says whether it is still the same one. An unreferenced
  vnode keeps its identity and pages on one of four free lists: at the
  tail if still cached, at the head if empty. `vn_rele()` picks the
  shorter of two lists by rotor.
- **Reuse policy:** `vn_alloc()` builds a new vnode while there are
  fewer than `vn_minvn` (512), or fewer free vnodes than vnodes in use.
  This is the policy IRIX actually applies, since its coin-flip branch
  never runs. Otherwise it tries up to four free ones, least recently
  freed first. It skips vnodes with dirty pages, and any whose file
  system refuses `VOP_RECLAIM` without waiting, so it may be called with
  a file system lock held.
- **Deviation:** no shaking yet. Vnodes are not handed back to the
  zone, so IRIX's epoch check is unnecessary. Shaking comes with the
  pager.
- **Inactivation:** `VOP_INACTIVE` returns `VN_INACTIVE_CACHE` or
  `VN_INACTIVE_NOCACHE`. A reference taken while it runs keeps the
  vnode off the free lists. A release during it (`VREINACT`) makes it
  run again.
- **Private vnodes** stay for file systems whose node is the only copy
  of the data (tmpfs). They are embedded and freed with the node, and
  their pages never go on the cache lists.
- **Name cache:** the System V references are replaced by IRIX-style
  soft references (pointer plus `v_number`). A hit takes a real
  reference with `vn_get()` under the cache lock, and a stale entry is
  dropped.
- **Page cache** (`<vm/pvn.h>`), per vnode as in IRIX: each vnode hashes
  its own pages by page number, in a table that doubles as the file
  grows (eagerly, rather than IRIX's lazy rehash).
  - A page is held while in use. A clean page nobody holds keeps its
    identity on its node's cache list, oldest first, where a lookup
    takes it back with no I/O.
  - The allocator recycles cache pages only when the buddy lists cannot
    satisfy a request, so free memory and the page cache are one pool.
  - Recycling follows the IRIX order inversion: mark the page and hold
    the vnode's cache under the node lock, then unhash it under the
    vnode lock.
  - Busy pages (being filled or written) make lookups wait.
- **Write-back:** dirty pages sit on their vnode's dirty list, and dirty
  vnodes on a global list. `bdflush` writes back vnodes dirty for 3
  seconds through `VOP_PUTPAGE`; sync, `fsync` and unmount write back
  everything. Unmount then reclaims the file system's cached vnodes
  before calling `VFS_UNMOUNT`.
- **ext2 file data** moved from the buffer cache to the page cache. It is
  read and written straight to the device, coalescing adjacent blocks.
  The buffer cache keeps metadata, directories and symbolic links.
  - The write path fills a page before allocating a block under it, so
    a new block reads as the hole it was.
  - Truncation zeroes the tail of the last page and tosses the pages
    past it before freeing blocks.
  - A block reallocated as file data has any stale buffer discarded
    (`binvalblk`).
  - In-core inodes now live as long as their vnode's identity.
    `ext2fs_reclaim` drops them, under `em_lock`. When the caller of
    `vn_alloc` already owns that lock (ext2's own `vget`), the reclaim
    proceeds under it, so one busy file system can reuse its own vnodes.
- **tmpfs** keeps file data in its private vnodes' page caches, ready
  for mapping later.
- **Not yet:** clustering and read-ahead, vnode shaking, and a pager to
  clean pages under memory pressure.

## User processes (2026-11-08)

- **System calls** enter through `syscall` and leave through `sysretq`.
  `iretq` is used instead whenever the frame no longer describes an
  ordinary user context (exec, or a non-canonical %rip), and whenever
  its %rcx and %r11 are not the %rip and %rflags that `sysretq` would
  leave in them: a context loaded by `setcontext` (how a signal handler
  returns) or a signal frame must give every register back. A handler
  that returned to code stopped by an interrupt used to find those two
  registers overwritten (fixed 2026-11-12). `md_syscall_return` keeps
  %r11 equal to the flags it sets, so ordinary calls stay on `sysretq`.
  - The native table uses UnixWare's numbers, with arguments in %rdi,
    %rsi, %rdx, %r10, %r8 and %r9.
  - Results follow the System V convention: on success the carry flag is
    clear and %rax and %rdx hold two values (%rdx defaults to the
    caller's own); on failure the carry is set and %rax holds the errno.
  - fork returns the other process's id, with 1 in %rdx in the child.
    getpid returns the parent's id in %rdx. The old wait returns the pid
    and status word.
  - Numbers without a handler fail with EINVAL; UnixWare would also post
    SIGSYS, which comes with signals.
- **The calls so far:** exit, fork and fork1, read, write, pread,
  pwrite, open, creat, close, lseek, dup, fcntl, ioctl (always ENOTTY),
  getdents, stat, lstat and fstat (a native 64-bit `struct stat`),
  link, unlink, rename, symlink, readlink, mkdir, rmdir, chdir, fchdir,
  chmod, access, umask, ftruncate, sync, exec and execve, wait and
  waitsys, brk, mmap, munmap, mprotect, getpid, getuid, getgid, time,
  gettimeofday, uname, and `__lwp_private`, which sets the %fs base.
- **Processes and LWPs** are separate structures from the start (SVR4).
  Each LWP runs on a kthread of its own that joins its process's
  fair-share time-sharing job on entry (see "LWPs" below).
  - Process 0 stands for the kernel; its children are the processes it
    starts with `proc_spawn()`, which are given the console on fds 0 to 2.
  - Orphans go to process 0, which reaps them as they exit; there is no
    init yet.
  - Without signal delivery, a fatal fault ends the process with the
    status of being killed by the signal UnixWare would send: SIGSEGV
    for an unmapped or protected address, SIGBUS for an object error,
    SIGFPE, SIGILL and SIGTRAP. A note goes to the console.
- **exec** runs static ELF-64 executables only.
  - Each `PT_LOAD` is a private file mapping, so the text is the page
    cache's own pages, and the bss is demand-zero.
  - The stack is all of `VM_STACK_SIZE` (8 MB), demand-zero, below
    `USRSTACK`, mapped at exec as in UnixWare.
  - The initial stack follows the AMD64 psABI, and the auxiliary vector
    is always present.
  - Errors after the new image replaces the old kill the process.
- **Address spaces** follow SVR4: a sorted segment list over a HAT,
  with the IRIX address-space mrlock (faults shared, layout changes
  exclusive).
  - `seg_vn` maps files and anonymous memory, private or shared. Shared
    writable file mappings are refused until pages written through a
    mapping can be written back.
  - Anonymous memory is the SVR4 anon layer: refcounted anons in
    per-segment maps. fork shares them and write-protects both sides;
    the first write copies.
  - Without swap an anon simply owns its page; page identity apart from
    swap (anonfs) comes with paging.
- **An anon page is never freed while mapped:** every path unloads a
  translation before releasing the anon behind it, the copy-on-write
  fault included. Unloading then tells a page-cache page (whose hold
  the mapping releases) from an anon page by the page's identity.
- **read and write fault their user buffer in** before calling the file
  system, so the copy inside cannot fault into `VOP_GETPAGE` while the
  file system holds its own locks.
- **Mapped page-cache pages** are held by their mappings. A file with
  mappings cannot be truncated (ETXTBSY), so truncation never meets a
  mapped page. A reverse map comes with the pager.
- **User page tables:** each address space has a PML4 sharing the
  kernel half. Its translations are non-global, so a %cr3 load flushes
  them. Each hat records which processors have it loaded, and
  invalidations go only to those.
  - Kernel threads run on the kernel's tables, so a dead address space
    is loaded nowhere.
  - SMEP is enabled where present. SMAP is not, since it needs
    `stac`/`clac` around the copy routines.
- **User state** is switched eagerly: FPU and vector registers with
  XSAVE (x87, SSE, AVX) or FXSAVE, a page per LWP, plus the %fs base.
  - The kernel is built without SSE, so the registers only ever hold user
    state.
  - An LWP's user trap frame sits at the top of its kernel stack, where
    TSS.RSP0 points while it runs.
- **copyin, copyout and copyinstr** check the range against the end of
  user space and set a per-thread recovery point. A fault there is
  resolved through the address space when it can be; otherwise the
  routine returns EFAULT.
- **NMI, double fault and machine check** decide on swapgs from
  `MSR_GSBASE`, since they can arrive between a swapgs and the return
  to user mode.
- **Tests:** small freestanding programs (`usr/tests`) are built with
  the host compiler against the kernel's own ABI headers and put in
  `/bin` of the test images.
  - `utest` checks the calls from the inside: fork and copy on write
    (both directions), exec with argv, envp and auxv, every fault kind,
    files, mmap, the FPU state across preemption, and TLS.
  - The kernel runs it alone and four at a time, twice. Afterwards the
    objects processes use (proc, lwp, file, as, seg, anon, hat, and so
    on) must be back to their earlier counts, pages may grow only by kmem
    high-water slack, and the file system must still unmount.
  - A kernel-side unit test drives the address-space layer directly:
    faults, a split by unmapping a hole, protection, and fork's
    copy-on-write.
  - `test-fault` and `test-apfault` now require the panic they provoke,
    so an unrelated panic can no longer pass as the expected one.
- **Not yet:** init and sessions, device special files (the console is
  a private vnode). (Resource limits came on 2026-11-12, and
  credentials on 2026-11-13, below.)

## Signals (2026-11-08)

- **Numbers and flags** are UnixWare's: 34 signals (`SIGCLD` 18, `SIGSTOP`
  23, `SIGCONT` 25, ...), the `sigaction` flag values, and the old
  multiplexed `signal` call (48) with its sigset, sighold, sigrelse,
  sigignore and sigpause bits. The structures (`sigset_t` of 128 bits,
  `struct sigaction`, `stack_t`, `siginfo_t`, `ucontext_t`) use native
  amd64 layouts.
- **Posting** follows UnixWare: a pending signal is not posted again and
  keeps its first `siginfo`; SIGCONT cancels pending stops and a stop
  cancels a pending SIGCONT; ignored signals are discarded.
- **Delivery** happens on every return to user mode (system call,
  exception, interrupt). The LWP takes SIGKILL first, then the
  highest-numbered pending signal it does not hold, as UnixWare's
  `signext` does; at most one handler frame is built per return.
  - The handler runs with its `sa_mask` and the signal itself held
    (unless `SA_NODEFER`). `SA_RESETHAND` resets the action, except for
    SIGILL, SIGTRAP and SIGPWR.
  - Default actions: terminate, ignore (SIGCONT, SIGCLD, SIGPWR,
    SIGWINCH, SIGURG, SIGWAITING, SIGLWP, SIGAIO), or stop (SIGSTOP,
    SIGTSTP, SIGTTIN, SIGTTOU).
  - There are no core files yet, so a fatal signal reports `CLD_KILLED`,
    which is what UnixWare reports when no core is written.
- **The handler frame** follows the psABI.
  - The handler is called as `handler(sig, siginfo_t *, ucontext_t *)`
    below the red zone or on the alternate stack. `siginfo` is passed
    only with `SA_SIGINFO`, and the handler starts with clean FPU state.
  - Its return address is the library trampoline registered as
    `sigaction`'s fourth argument (after UnixWare's `p_sigactret`), or a
    non-canonical value. The trampoline returns through `setcontext`
    (call 100) with the context from %rbx.
- **setcontext** validates everything it loads: user selectors, user-only
  rflags bits, a user %rip and %fs base, and an FPU image cleaned of
  any XSAVE header or MXCSR bits the processor would refuse.
  `__lwp_private` checks its address the same way.
- **Interrupted calls** return EINTR, or ERESTART (91) when the handler
  has `SA_RESTART`; the library reissues the call, as UnixWare's does.
  - A wait rescans its children before giving up, so the SIGCHLD of the
    very child it waits for cannot hide that child's exit.
  - A stop and continue during pause, sigsuspend or wait just resumes
    the wait.
- **Interruptible sleeps** (`sv_wait_sig`, `sv_wait_mutex_sig`) record
  the object's lock, so a poster can take the sleeper off its queue.
  - The sleeper re-checks for a signal after queueing itself, with full
    barriers on both sides, so a signal arriving as it falls asleep is
    never lost.
  - Only objects that live as long as the sleeper's process are slept on
    this way.
- **Faults** raise their signals with `siginfo`. A page fault with no
  mapping gives SIGSEGV `SEGV_MAPERR`, one on a protected page
  `SEGV_ACCERR`, and an object error SIGBUS `BUS_OBJERR`; divide gives
  SIGFPE `FPE_INTDIV`, and so on. A fault that is held or ignored gets
  its default action back first.
  - An unassigned system call number posts SIGSYS, then fails with
    EINVAL (127, `invsys`, only fails).
- **Job control:** stopped processes wait until SIGCONT or SIGKILL.
  waitsys reports `CLD_STOPPED` and `CLD_CONTINUED` with `WSTOPPED` and
  `WCONTINUED`.
  - SIGCHLD follows UnixWare's two flags. Without `SA_NOCLDSTOP` the
    parent hears of stops (process 0 starts that way and fork copies it).
    SIGCHLD ignored, or `SA_NOCLDWAIT`, means no zombies, and any
    zombies already present are freed.
- **Also:** alarm and pause, sigsuspend, sigpending, sigaltstack,
  getcontext and setcontext, kill to a process, a group, or everyone,
  and `pgrpsys` (getpgrp, setpgrp, getsid, setsid, getpgid, setpgid).
  exec resets caught signals and clears both SIGCHLD flags, as in
  UnixWare.
- **Not yet:** `/proc` tracing, core files, and SIGXFSZ on EFBIG.
  (Permission to signal came with credentials, 2026-11-13; terminal job
  control and orphaned process groups on 2026-11-14.)

## LWPs (2026-11-08)

- **The model:** UnixWare's. A process holds a list of LWPs, each with
  its own id (from 1, never reused within the process), signal mask,
  pending signals, alternate stack, and FPU state; everything else is
  shared. The list and every LWP's state flags are under `p_siglock`.
- **The calls,** with UnixWare's numbers: `_lwp_create` (from a
  `ucontext_t`, with `LWP_DETACHED` and `LWP_SUSPENDED`), `_lwp_exit`,
  `_lwp_wait` (one LWP, or any undetached one with id 0), `_lwp_self`,
  `_lwp_info`, `_lwp_kill`, `_lwp_suspend`, `_lwp_continue`,
  `_lwp_sema_wait`, `_lwp_sema_trywait`, `_lwp_sema_post`, sigwait (and
  so sigtimedwait), fork1, and forkall; fork is forkall, as in UnixWare.
- **Process-wide changes** go through one of two operations:
  - `proc_single()` makes the caller the only LWP: exit and exec mark
    the others for destruction and wait until they are gone. Only one
    LWP can win; the loser of two racing calls exits itself.
  - `lwp_rendezvous()` parks every other LWP at its next return to
    user mode (or in an interruptible sleep, which it leaves with
    `ERESTART`), so that forkall copies a quiet set of user contexts.
    Suspension uses the same parking.
- **Exit:** the last LWP to exit ends the process with status 0. An
  exiting LWP becomes a zombie for `_lwp_wait` unless detached; it
  stays marked as exiting until its kthread is done with it, so a
  waiter never sees it neither live nor waitable.
- **Signals:** a signal to the process goes to an LWP waiting for it in
  sigwait, else to one that has it unmasked, else stays pending on the
  process. Faults and `_lwp_kill` go to one LWP. Stop and continue act
  on every LWP; a stop is complete, and the parent told, once all are
  parked.
- **Address spaces:** faults by several LWPs on one page keep each
  other's translation (they only change its protection), and a loser of
  a race to map a file page drops its hold; mmap without `MAP_FIXED`
  finds and maps its gap under one hold of the address space lock.
- **LWP semaphores** keep their count in user memory, changed by the
  kernel under a mutex of a bucket hashed by address space and address.
- **Not yet:** SIGWAITING and SIGLWP from the kernel (for a
  user-level thread library), bound and unbound LWPs, per-LWP
  scheduling classes and `priocntl`, a system time in `_lwp_info` (all
  of an LWP's ticks count as user time), and
  process-shared semaphores across address spaces.

## The C library (2026-11-09)

- **mlibc**, from the `belka` branch of the fork at
  `github.com/Kazuo-Kuroi/mlibc`, which starts from upstream's `v7.x`
  release branch (7.0.0) rather than `master`. Belka carries it as the
  submodule `contrib/mlibc`; the port lives in the fork, in
  `sysdeps/belka` (the system calls) and `abis/belka` (the ABI headers),
  so that it can be rebased onto later releases and offered upstream.
  It is mlibc's own code, under its MIT licence and copyright; the
  clean-room rules apply to the SVR4 and IRIX sources, not to it.
- **Language:** mlibc is C++23 and builds with meson. That stays inside
  mlibc; the kernel and Belka's own programs remain C99 and GNU make,
  which runs mlibc's build with muon (0.7 or later: earlier versions
  ignore cross files) and samurai. muon fetches mlibc's subprojects
  with git and installs every file each time, so the sysroot is
  installed afresh only when `libc.a` was rebuilt. mlibc builds with GCC
  and with Clang, each from a cross file in `tools/mlibc`, with the same
  compiler as the kernel (`TOOLCHAIN`). It needs GCC 14 or later in
  practice (C++23 explicit object parameters), though its build checks
  only for 13; `GCC_VERSION` picks a version, and a host's settings may
  go in an untracked `config.mk`. For a bare ELF target Clang does
  not predefine `_GNU_SOURCE` in C++, as it does for Linux, so the Clang
  cross file passes it.
- **Linking:** with the link-editor run directly rather than through
  the compiler driver, dynamically by default (see Shared libraries
  below). Programs link mlibc's `crt1.o` and `crti.o`, a start object,
  `libc.so` (or `libc.a`), the compiler's run-time library (libgcc, or
  compiler-rt's builtins), an end object, and `crtn.o`. With the illumos
  link-editor the start and end objects are Belka's own (`usr/lib/crt`);
  with GNU ld they are the compiler's (GCC's `crtbeginT.o`, or
  `crtbeginS.o` for a position-independent program, or compiler-rt's
  `crtbegin`). Test images get static programs stripped of debug
  information.
- **ABI headers** (`abis/belka`) give the kernel's values and layouts:
  UnixWare error numbers, signal numbers and flags, `open` flags and
  `fcntl` commands, `waitsys` options, mapping flags, and the native
  `struct stat`, `sigaction`, `siginfo_t`, `stack_t`, `ucontext_t`, and
  `utsname`. Headers for facilities Belka lacks (sockets, terminals,
  resource limits) point at mlibc's Linux ones until the kernel defines
  its own. `tools/abicheck` checks at build time that every shared
  constant, size, and offset agrees with the kernel's headers.
- **Names the kernel lacked:** `ENOTSUP` (48), `EDQUOT` (49), and
  `EOWNERDEAD` (58) take the numbers later System V releases gave them;
  `ENOTRECOVERABLE` takes 100, which UnixWare left free. Linux-only names
  that mlibc's `strerror` lists take values from 4080, which no kernel
  error uses. `SI_QUEUE`, `SI_TIMER`, `SI_ASYNCIO`, and `SI_MESGQ` take
  the later System V values, and `SI_TKILL` is `SI_LWP`. There are no
  real-time signals: `SIGRTMIN` is 35 and `SIGRTMAX` 34, an empty range.
  mlibc's thread cancellation signal is `SIGLWP`.
- **What the library does itself:** `O_CLOEXEC`, `O_DIRECTORY`, and
  `O_NOFOLLOW` (values from later System V) are carried out around
  `open`; `F_DUPFD_CLOEXEC` and the `dup` flags likewise;
  `AT_EMPTY_PATH` becomes the call on the descriptor; `readdir`
  rewrites the kernel's records, which have no `d_type`; `waitpid` runs
  over `waitsys` and builds the status word from the `siginfo_t`; every
  clock reads `gettimeofday`; `isatty` asks for `TCGETA`. System calls that fail with `ERESTART` are reissued, and
  signal handlers return through a trampoline passed with every
  `sigaction`, as the kernel expects.
- **The thread pointer** is the `%fs` base, set with `__lwp_private`, so
  `errno` and other thread-local data work from the first instruction
  of `main`.
- **Upstream fix:** mlibc 7.0.0's `recvfrom` instantiated the `Recvfrom`
  sysdep even for systems without one; the fork calls it through
  `sysdep_or_enosys` (also unfixed on `master`).
- **Tests:** `usr/tests/libctest.c`, built against mlibc, checks
  start-up and the environment, formatting and conversions, the heap,
  stdio on files, `stat`, errno, directories, `fork`, exit and signal
  statuses, `execve`, `abort`, handlers, masks and pending signals, a
  fault handled with `longjmp`, the clocks, and `uname`. The process
  self-test runs it.
- **The test programs** all build against mlibc (2026-11-13; they had a
  small C library of their own, libt, until then). `usr/tests/rawsys.h`
  gives those that test the kernel's own interfaces (LWPs and their
  semaphores and address waits, `fork1`/`forkall`, contexts, the break,
  System V `signal`, `sigwait`) inline system calls that return error
  numbers and never touch `errno`, since an LWP made with `_lwp_create`
  has no thread block of mlibc's (its `%fs` base is 0) and must not
  call into the library. The interval timers came on 2026-11-14 (see
  Interval timers below).
- **Later:** the `*at` calls, `chown`, `poll`, named FIFOs and resource
  limits came on 2026-11-12; `ppoll`, `pselect` with a mask,
  `getrusage`, and terminals with `termios` on 2026-11-13; see below.

## POSIX threads (2026-11-09)

- **Model:** one POSIX thread is one LWP (1:1). mlibc's `pthread_create`
  maps a stack (2 MB unless asked otherwise, with a guard of
  inaccessible pages below it) and starts an LWP with `_lwp_create` in a
  context giving its entry point, stack, and `%fs` base (the new TCB).
  Threads are created `LWP_DETACHED`: `pthread_join` waits on a word in
  the TCB, so the kernel need not keep exited LWPs for `_lwp_wait`.
  When the last LWP exits, the process exits with status 0.
- **Address waits** (decided 2026-11-09, native rather than UnixWare's
  user-sync calls): `_lwp_addr_wait(int32_t *addr, int32_t expected,
  const timespec *rel)` sleeps while `*addr` holds `expected`, until a
  wake, a signal (`EINTR`), or the relative timeout (`ETIMEDOUT`), and
  returns `EAGAIN` at once if the word differs. `_lwp_addr_wake(addr,
  n)` wakes up to `n` waiters (all if `n <= 0`) and returns how many.
  They take 181 and 182, which UnixWare left free. UnixWare's own calls
  (`prepblock`, `block`, `rdblock`, `unblock`, `cancelblock`, 183 to
  187) stay reserved for the i386 personality and its `libthread`:
  they keep a waiter flag the kernel writes at an address the caller
  names, and reader-writer classes, which fit mlibc's single-word
  protocol badly.
- **How they work:** waits are keyed by address space and address and
  share the LWP semaphores' hash buckets. A waiter is queued under the
  bucket's mutex from the check of its word on, so a waker, which marks
  waiters under the same mutex, either finds it queued or changed the
  word before the check; no wake is lost. Waiters sleep on the bucket's
  sync variable and go back to sleep until marked, so a wake for another
  address in the bucket costs only a recheck. Only a process's own LWPs
  share keys; process-shared waits across address spaces are not done.
- **Timeouts** are a callout that sets `l_timedout` and nudges the LWP
  as a signal would, so a timeout that fires just before the sleep is
  not missed. `lwp_timeout_start` and `lwp_timeout_stop` serve both the
  address waits and `sigtimedwait`; stopping cancels the callout (waiting
  for it if it is running) before clearing `l_timedout`. `sigtimedwait`
  used to clear it first, which a late callout could undo, leaving every
  later sleep of that LWP cut short.
- **mlibc's futexes** are these calls; `nanosleep` and the other sleeps
  are a timed address wait on a word no one wakes, with what is left
  after a signal worked out from the time of day. `pthread_kill` is
  `_lwp_kill`; thread cancellation uses `SIGLWP`.
- **Cancellation is deferred-only in effect:** no system call is marked
  cancellable yet, so a cancellation request takes effect at the next
  cancellation point (`pthread_testcancel`, joins, waits), not inside a
  blocked call, and never wherever a signal happens to land.
- **Upstream fix:** mlibc 7.0.0's `pthread_create` cleared the TCB's
  cancellation bits after the TCB was made with cancellation enabled,
  so no thread could be cancelled; the fork starts threads with it
  enabled (also unfixed on `master`).
- **Tests:** `lwptest` checks the address waits directly (a lock built
  on them under contention, wake counts, timeouts, `EAGAIN`, `EINVAL`,
  `EFAULT`, and interruption by a handler); `threadtest`, built against
  mlibc, checks creation and join, detached threads, mutexes (also timed
  and recursive), condition variables and their timeouts, semaphores,
  read-write locks, barriers, `pthread_once`, thread-specific data and
  its destructors, per-thread `errno`, `pthread_kill`, cancellation,
  `nanosleep`, and a process that exits while its threads are blocked.
- **Not yet:** cancellation inside blocked system calls, thread
  priorities and scheduling attributes (`priocntl`), CPU affinity,
  process-shared mutexes and condition variables, robust mutexes,
  freeing exited threads' stacks and TCBs (mlibc keeps them), and
  SIGWAITING for a many-to-many library.

## The link-editor (2026-11-09)

- **The illumos link-editor**, from the repository `illumos-ld`
  (carried as the submodule `contrib/illumos-ld`): `ld`, `libld`,
  `libelf`, `liblddbg`, `libconv`, and the `sgsmsg` tool, imported
  unchanged from illumos-gate `d493fab2c163` under their own paths, with
  Belka's changes as separate commits and a GNU make build for other
  hosts in `port/`. It is CDDL, with the notices kept and a "Portions
  Copyright" line in each file Belka changed; the build layer is MIT.
- **It is built for the host** with `HOSTCC` (GCC or Clang) and GNU m4,
  into `build/tools/illumos-ld`, before anything is linked. Solaris
  interfaces the sources expect come from `port/compat`: types and
  macros, Solaris threads over POSIX threads, `sysinfo` and
  `resolvepath`, and illumos's ELF headers, linked in so that they
  shadow only the host's matching ones. No Perl or Python is needed.
- **Changes to libld**, each needed by a system that illumos is not
  (static, Limine-booted, with mlibc's runtime linker):
  1. the debug stream is not initialized with `stderr`, which need not
     be a constant;
  2. TLS is allowed in static executables (illumos has none): every
     access model relaxes to local-exec, as in a dynamic executable;
  3. the `__tls_get_addr` call in a relaxed TLS sequence is dropped when
     that function is defined locally, instead of being relocated over
     the rewritten code;
  4. static executables map their ELF and program headers, so that the
     kernel can pass `AT_PHDR`;
  5. a segment with a mapfile `VADDR` keeps its mapfile `ALIGN` (Limine
     rejects alignment 0);
  6. executables' segments get physical addresses equal to their
     virtual ones (Limine loads by them);
  7. initialization and finalization arrays are ordered by priority
     (`.init_array.N`), as GNU ld does, which mlibc's start-up relies
     on.
  8. local-exec TLS offsets follow the AMD64 psABI (see Shared
     libraries below).
- **The kernel** is linked with a mapfile (`kernel.map`, beside the GNU
  `kernel.ld`). `kernel_begin.S` and `kernel_end.S`, linked first and
  last with either linker, mark the segments' bounds (`kernel_start`,
  `etext`, `erodata`, `kernel_end`); `tools/check-kernel.sh` checks after
  every link that there are three page-aligned segments, R-X, R--, and
  RW-, with physical addresses equal to virtual ones, and the symbols at
  their bounds.
- **Programs** are linked with `-dn` and `tools/ld/program.map`, which
  makes their data non-executable, between `usr/lib/crt/crtbegin.o` and
  `crtend.o`. Those mark the initialization arrays (the start labels in
  sections of priority 0, so that the priority order puts them first)
  and define `__dso_handle`, which GNU ld and the compiler's crtbegin
  would otherwise provide; compiler-rt's crtbegin also has two
  `.eh_frame` sections libld will not merge.
- **mlibc changes:** static start-up finds the program headers in the
  auxiliary vector, not through `__ehdr_start`, which only GNU ld and
  lld define (a fallback remains); GNU IFUNCs are off, since libld does
  not support them (the generic string functions are used); and GCC
  builds it with `-fno-gnu-unique`, since libld does not know the
  `STB_GNU_UNIQUE` binding.
- **Not yet:** driving the link-editor through the compiler drivers (an
  `x86_64-belka` target); C++ name demangling in its messages; and a
  native `ld` on Belka itself.

## Time, pipes, and file calls (2026-11-10)

- **The high-resolution clock:** `gethrtime()` gives nanoseconds since
  the clock started, from the invariant TSC, scaled by a 32-bit
  multiplier and shift with the product split at 32 bits so that it
  cannot overflow (plain C99, no 128-bit integers). A processor whose
  TSC is not invariant falls back to the clock tick. Processors' TSCs
  may differ slightly, so the latest value any processor read is kept
  (compare-and-swap), and no read returns an earlier one.
  `gethrestime()`, the time of day, is boot time plus `gethrtime()`, so
  it too is fine-grained. QEMU's `-cpu max` leaves the invariant TSC
  out; the test configuration asks for it (`QEMU_CPU`), and the qemu64
  model tests the fallback.
- **Clocks:** `clock_gettime` (210) and `clock_getres` (211), with the
  POSIX clock numbers the C library uses: the time of day, monotonic
  time (the raw, coarse, and boot-time ones are the same clock), and
  the process's and the LWP's processor time. Resolution is 1 ns from
  the TSC, else a tick; processor time is counted in ticks.
- **Processor time:** a process's is its exited LWPs' ticks (`p_ticks`,
  added as each exits) plus its live LWPs'; a reaped child's total is
  added to its parent's `p_cticks`. `times` (UnixWare's 43) reports
  them in ticks, all as user time for now, and returns the ticks since
  boot; the C library's `sysconf(_SC_CLK_TCK)` is `HZ` (100).
- **Timeouts** are rounded up to ticks and one tick added, since a
  callout set for n ticks runs at the n-th tick boundary, which can be
  most of a tick sooner; the nanosecond clock showed `nanosleep`
  returning early.
- **`sched_yield`** (212) lets other threads of the same priority run.
- **Pipes** (`pipe`, UnixWare's 42, returning the two descriptors in
  `%rax` and `%rdx`): one private `VFIFO` vnode (`sys/fs/fifofs`), opened
  for reading on the first descriptor and for writing on the second
  (POSIX pipes, not System V's two-way STREAMS ones). Its data is a
  16 KB ring buffer; writes of up to `PIPE_BUF` (4096) bytes are atomic;
  reading with no writer left is end of file; writing with no reader
  sends `SIGPIPE` to the writing LWP and fails with `EPIPE`.
  `O_NONBLOCK` makes a call that would wait fail with `EAGAIN`, and
  `O_NDELAY` return 0, as in System V. Seeking on a pipe fails with
  `ESPIPE`. Named FIFOs share the node (below).
- **File calls with UnixWare's numbers:** `fsync` (58), `fchmod` (93),
  and `readv` and `writev` (121, 122; at most 1024 vectors). The C
  library also gained `chmod`.
- **`getcwd`** is the C library's, walking up through `..` as System V's
  does: each parent's entry with the child's device and inode names it,
  and across a mount point every entry is examined, since a directory's
  inode numbers are not those of what is mounted on it.
- **A signal-return race fixed:** `setcontext` copied the new
  floating-point state into the LWP's save area before disabling
  interrupts, so a switch away in between saved the handler's live
  registers over it. It now copies and loads with interrupts off.

## Shared libraries (2026-11-10)

- **Exec and the program interpreter:** a program may name an
  interpreter (`PT_INTERP`, a path of at most `MAXPATHLEN`). Exec maps it
  at `VM_INTERP_BASE` and starts there, with `AT_BASE` its load address
  and `AT_ENTRY` the program's entry. The interpreter must be executable,
  `ET_DYN`, and name no interpreter of its own. A position-independent
  program (`ET_DYN`) is loaded at `VM_PIE_BASE`.
- **The shared C library** is mlibc built a second time, as its runtime
  linker and C library, in `build/mlibc-shared-$(TOOLCHAIN)`.
  - The illumos link-editor links both, whatever `LINKER` says.
  - mlibc links through the compiler driver with GNU ld's options, so
    the cross file (`tools/mlibc/*-shared.cross`) names
    `tools/ld/illumos-cc` as the compiler. It compiles with the real
    compiler and links with the illumos options (`-G`, `-h`, `-R`,
    `-z defs`, `-z allextract`, `-z rescan-start`, and so on).
  - A GNU version script becomes a mapfile, its patterns expanded
    against the symbols the objects define, since mapfiles have no
    wildcards. That is how `libc.so` hides its C++ symbols.
  - Asked which linker it is (meson's `cc -Wl,-v`), the wrapper says it
    takes GNU ld's options.
  - The C library takes its runtime helpers from libgcc or compiler-rt.
- **Names and places** (decided 2026-11-10): SVR4's versioned names in
  `/usr/lib64`. The runtime linker is `/usr/lib64/ld.so.1`, which every
  dynamic program names as its interpreter; the C library is
  `libc.so.1`, with `libc.so` for the link-editor. mlibc's fork gained a
  `soversion` option for this, and its search path is `/usr/lib64`.
  `libc.a` and the start-up objects live there too.
- **The sysroot** holds both builds: the static one installed first,
  the shared one over it. A build is configured afresh when its options
  or its cross file change; the cross files are made from
  `tools/mlibc/*.cross` with the toolchain's `CC` and `CXX` filled in.
- **Programs built against mlibc are dynamic by default.** `libctest`,
  `threadtest` and `dyntest` are dynamic; `libctest-static` is
  `libctest` linked statically, so the static start-up stays tested.
  - The link-editor finds a shared object's own dependencies
    (`libc.so.1` needs `ld.so.1`) in the sysroot: illumos ld's `-Y P`
    makes it the default directory, GNU ld's `-rpath-link` the same.
    The installed `libc.so.1` has no run path of its own.
- **The test images** (12 MB) carry `ld.so.1` and `libc.so.1` in
  `/usr/lib64`, and the process self-test links the root's `/usr/lib64`
  to the image's, so programs use their real interpreter path.
  - The copies are linked a second time with the link-editor's `-s`,
    not stripped. GNU strip rewrites the illumos link-editor's dynamic
    objects wrongly: it moved `ld.so`'s data segment and emptied a
    program's `PT_INTERP`.
  - Dynamic programs are copied unstripped.
- **`AT_EXECFN`** (31, the number the C library's headers use): exec
  copies the program's path, as it was given, onto the new stack. The
  runtime linker expands `$ORIGIN` in the program's own run path from it.
- **Mapped objects:** the runtime linker reserves an object's range
  with an inaccessible anonymous mapping, at an address the kernel
  chooses, and maps each segment from the file over it with
  `MAP_FIXED`, private and copy-on-write, so text is the page cache's.
  It no longer reads objects into anonymous memory at fixed addresses.
- **mlibc changes**, for the illumos link-editor's output:
  - OS-specific dynamic tags, and those of the value range such as
    `DT_CHECKSUM`, are ignored, in `ld.so`'s check of itself too.
  - `DF_ORIGIN` and `DF_1_ORIGIN` are accepted.
  - The `PT_SUNW_UNWIND` (older illumos output) and `PT_SUNWSTACK`
    segments are ignored.
  - A relocation against a local symbol uses that symbol in its own
    object, with no lookup. libld gives TLS module relocations the TLS
    section's symbol, where GNU ld uses symbol 0.
  - `ld.so` finds its own ELF header at its base address when
    `__ehdr_start` is undefined.
- **libld changes:**
  - An executable's local-exec TLS offsets are its TLS size rounded to
    the segment's own alignment. That is the AMD64 psABI rule, which GNU
    ld and mlibc's runtime linker follow. libld rounded to 16 bytes, as
    Solaris's runtime linker expects. Static programs had matched only
    because their TLS size was a multiple of 16.
  - The `.eh_frame_hdr` segment is `PT_SUNW_EH_FRAME`, the value the
    AMD64 psABI requires (`PT_GNU_EH_FRAME`), which unwinders other than
    illumos's look for, rather than the older `PT_SUNW_UNWIND`.
  - **Position-independent executables** (`-z type=pie`, after Oracle
    Solaris's option; illumos has none). One is a shared object that
    names an interpreter (the default one if `-I` is not given), marked
    `DF_1_PIE`. Its own references bind to its own definitions, as with
    `-B symbolic`, since nothing can interpose on a program's symbols;
    like an executable it may leave nothing undefined, and its TLS is
    the first static block, so the executable's TLS models and
    relaxations apply. It asserts `-z text`, and takes no soname.
  - It makes no copy relocations, so position-independent code must
    reach other objects' data through the GOT: clang's does, and GCC
    needs `-mno-direct-extern-access` (`LIBC_PIE_CFLAGS`).
- **Tests:** `usr/tests/dyntest.c` checks:
  - that the interpreter ran, and `AT_EXECFN`;
  - calls and data into `libc.so.1`: stdio, the heap, errno, and the
    environment;
  - `libdt.so.1` (`usr/tests/libdt.c`, a library of the tests' own, in
    the images' `/lib`), found through the run path `$ORIGIN/../lib`:
    its functions, its data (which the program copies into its own),
    its constructor, and calls from it into `libc.so.1`;
  - the program's, libc's and libdt's TLS across threads;
  - `dlopen` and `dlsym`, of `libc.so.1` and `libdt.so.1`;
  - `fork`, constructors, and exit handlers.

  `dyntest-pie` is the same program, position-independent, loaded at
  `VM_PIE_BASE`. With `LINKER=gnu`, GNU ld links them, and `libdt.so.1`,
  against the same `libc.so.1`.
- **Not yet:** shared libraries of Belka's own beyond the test one,
  `libc.so.1`'s versioning of its symbols (mapfile versions), lazy
  and lazy binding (mlibc binds everything at load). Set-ID programs
  get `AT_SECURE`, and mlibc's dynamic linker then ignores
  `LD_LIBRARY_PATH` and `LD_PRELOAD` (2026-11-13).

## Named FIFOs (2026-11-12)

- **Making one:** `mknod` (UnixWare's 14) with `S_IFIFO` in the mode;
  other types fail with `EINVAL` until there are device files. tmpfs and
  ext2 create FIFO nodes (`EXT2_FT_FIFO` in the directory entry). The C
  library's `mkfifo` is `mknod`, as in System V.
- **Opening one:** the file system's `VFIFO` vnode holds only the name
  and attributes. `vn_open` checks access on it, then trades it for a
  fifofs node with `fifo_vp()`: one node per file system vnode while
  any process has the FIFO open (a list under a spin lock), holding the
  file system's vnode. The node is a pipe's, so reading and writing,
  `PIPE_BUF` atomicity, `SIGPIPE` and the non-blocking behaviour are
  the same; attributes, `chmod` and access checks go to the file
  system's vnode, and `st_size` is the bytes held.
- **Open semantics (POSIX):** a reader waits for a writer, and a writer
  for a reader; each waits until the other side is open or has been
  opened since it began waiting. With `O_NONBLOCK` or `O_NDELAY` a
  reader goes ahead, and a writer with no reader fails with `ENXIO`.
  Opening for both never waits. An interrupted open undoes its count.
- **Data** is discarded once neither end is open; the node goes with
  the last reference.

## Owners (2026-11-12)

- **`chown` (16), `fchown` (94) and `lchown` (130)**, UnixWare's
  numbers, set a file's owner and group through `VOP_SETATTR`; an id of
  `(uid_t)-1` keeps that one. ext2 stores ids past 16 bits in the high
  halves. Who may change an owner, and the set-ID bits it clears, came
  with credentials (2026-11-13). The C library's `Fchownat` maps onto
  the three calls.

## poll (2026-11-12)

- **`poll`** (UnixWare's 87) with System V's event bits, which mlibc's
  Belka ABI now carries too (`POLLWRNORM` is `POLLOUT`, `POLLWRBAND`
  0x100). `VOP_POLL(vp, events, &revents, &head, cr)` reports which
  events hold now and hands back the object's `struct pollhead`; files,
  directories and the console are always ready (`fs_poll`).
- **No lost wakeups:** a pass that finds nothing registers the caller on
  every head, then looks once more; anything after the registration
  comes through `pollwakeup()`, which marks each registered poller fired
  and wakes it. The poller's state is on its stack, reachable only from
  the heads, and it unregisters before returning; its files stay held,
  so no head goes away under it. Locks: `ph_lock`, then `ps_lock`.
- **Results:** a descriptor open one way sees only that way's events;
  `POLLERR`, `POLLHUP` and `POLLNVAL` come unasked. Pipes and FIFOs are
  readable with data, hung up with no writer (a named FIFO once a writer
  has come and gone), writable with room for `PIPE_BUF` bytes, and in
  error with no reader. A signal ends the wait with `EINTR`, never a
  restart, as POSIX asks; the timeout is milliseconds, rounded up to
  ticks plus one.
- **`select`** is the C library's, on `poll`: readable is `POLLIN` or a
  hang-up or error, writable `POLLOUT` or an error, exceptional
  `POLLPRI`. It now goes through `pollsys` (2026-11-13, below).

## Resource limits (2026-11-12)

- **`getrlimit` (129) and `setrlimit` (128)**, UnixWare's numbers, with
  System V's resources: CPU 0, FSIZE 1, DATA 2, STACK 3, CORE 4, NOFILE
  5, VMEM 6 (`RLIMIT_AS`). Limits are 64-bit and `RLIM_INFINITY` is all
  ones, as mlibc has it; mlibc's Belka ABI gained `resource.h` with these
  numbers. A soft limit above its hard one is `EINVAL`; only the
  superuser may raise a hard limit (`EPERM`). fork copies the limits.
- **Defaults:** everything unlimited but the stack (soft 8 MB, the
  space set aside for it) and open files (1024, `NOFILE_MAX`, which also
  caps whatever is set).
- **Enforcement:**
  - NOFILE: no descriptor at or above the soft limit: `EMFILE` when none
    is free, `EINVAL` for `F_DUPFD`, `EBADF` for `dup2`.
  - FSIZE: System V's idea of a write limit in the `uio` (`uio_limit`):
    tmpfs and ext2 stop a write there, a write that can do nothing
    fails with `EFBIG` and sends `SIGXFSZ` to the writing LWP, and so
    does `ftruncate` past it.
  - DATA: the break may not grow past it.
  - VMEM: `mmap` and the break may not take the mapped total past it
    (`as_size()`, the sum of the segments; a `MAP_FIXED` mapping over
    existing ones is counted in full).
  - STACK: exec maps the soft limit, kept between 1 MB (room for the
    arguments, so exec never fails after the old image is gone) and
    `VM_STACK_SIZE`.
  - CPU: `userret()` compares `lbolt` with the process's next check
    time, set only while a limit is finite, and about once a second
    then sums the process's ticks: `SIGXCPU` past the soft limit (each
    second), `SIGKILL` past the hard one.
  - CORE is kept for when there are core files.

## The *at calls (2026-11-12)

- **Calls:** UnixWare 2 has none, so they are Belka's own, numbered
  after `yield`: `openat` 213, `mknodat` 214, `mkdirat` 215, `unlinkat`
  216, `renameat` 217, `linkat` 218, `symlinkat` 219, `readlinkat` 220,
  `fstatat` 221, `fchownat` 222, `fchmodat` 223, `faccessat` 224. Each
  takes its arguments in the POSIX order.
- **Flags** (`<sys/fcntl.h>`) take the values later System V gave them:
  `AT_REMOVEDIR` 0x1, `AT_SYMLINK_FOLLOW` 0x2, `AT_EACCESS` 0x4,
  `AT_SYMLINK_NOFOLLOW` 0x1000. `AT_FDCWD` is -100, the value mlibc's
  Belka ABI already had. A flag a call does not take is `EINVAL`.
- **Lookup:** a relative path starts at the directory open on the
  descriptor, held for the call; `lookuppn()` always had a start
  directory. An absolute path ignores the descriptor, and `AT_FDCWD`
  means the current directory. A descriptor that is not open is
  `EBADF`, and one that is not a directory `ENOTDIR`. Search permission
  on it is checked as on any directory of a path.
- **Kernel interface:** the path functions in `vfs_vnops.c` became
  their *at forms (`vn_openat`, `vn_removeat`, `vn_linkat` with a follow
  flag, and so on); the plain names are inline wrappers that pass NULL,
  the current directory. The old system calls are the new ones with
  `AT_FDCWD`.
- **Details:** `linkat` follows a last link in the existing path only
  with `AT_SYMLINK_FOLLOW`, as `link` never does. `fchmodat` with
  `AT_SYMLINK_NOFOLLOW` fails with `EOPNOTSUPP` on a link, which has no
  mode of its own, and changes anything else. `faccessat` with
  `AT_EACCESS` checks with the effective ids rather than the real ones.
- **C library:** every `*at` sysdep calls the kernel; `open`, `stat`
  and `readlink` go through the *at calls. mlibc declares `renameat`
  only for POSIX 2008, which `_DEFAULT_SOURCE` does not imply there.

## pollsys and getrusage (2026-11-13)

- **`pollsys(fds, nfds, timeout, mask)`** (225, Belka's own, after the
  Solaris call of that name) is `poll` with a `struct timespec` timeout
  (NULL: none) and a signal mask held for the wait alone (NULL: the
  mask stays). The mask goes in as `sigsuspend` puts its own: the old
  one is kept in `l_sigoldmask`. A wait that a signal ends (`EINTR`)
  leaves the old mask to the signal's delivery, which restores it after
  the handler, so the signal is taken under the wait's mask and no
  other slips in between. A wait that ends any other way puts the old
  mask back before returning. The timeout is ticks as for the other
  timed waits: rounded up, plus one.
- **The C library:** `ppoll` is `pollsys`, and `select` and `pselect`
  build their `pollfd` array and call it with the timeout and mask as
  given. mlibc declares `ppoll` only for POSIX 2024 or `_GNU_SOURCE`.
- **`getrusage(who, struct rusage *)`** (226, Belka's own; UnixWare 2
  has none) for `RUSAGE_SELF` and `RUSAGE_CHILDREN` (the children
  reaped). Only the times are kept, and all processor time counts as
  user time, as it does for `times`; the other fields are zero. The
  kernel's `struct rusage` has mlibc's layout.

## Credentials (2026-11-13)

- **`struct cred`** (SVR4): real, effective, and saved user and group
  ids and up to `NGROUPS_MAX` (16, as in UnixWare) supplementary groups,
  reference counted and never changed once shared; a change makes a
  copy (`crdup`) and installs it. Privilege is an effective user id of
  0; there is no finer privilege model yet.
- **Where they live:** the process's `p_cred` is replaced only by the
  process itself, under `p_lock`, with the pointer swapped under a
  small spin lock, `p_crlock`, so `kill` can take a reference to
  another process's credentials. Each LWP keeps its own reference,
  `l_cred`, brought up to date on entry to every system call (UnixWare
  does the same), so one call sees one set throughout and needs no lock:
  `CRED()`. An open file keeps the credentials it was opened with
  (`f_cred`); kernel threads use `kcred`.
- **Calls,** with UnixWare's numbers: `setuid` 23, `setgid` 46,
  `setgroups` 91, `getgroups` 92, `setegid` 136, `seteuid` 141;
  `getuid` and `getgid` return the real id and the effective one in the
  second register. POSIX rules with saved ids: the superuser's
  `setuid` sets all three, `seteuid` the effective one only; anyone
  else may set the effective id to the real or saved one. Ids above
  `0x7fffffff` are `EINVAL`. `setreuid` and the `setres*` calls are
  not provided.
- **File permissions:** the file systems check a file's own bits with
  `vaccess()` (owner, then group including supplementary groups, then
  others; the superuser may do anything but execute a file no one may
  execute). The rest is common, in `vfs_vnops.c` and `lookuppn()`:
  search permission on every directory of a path, checked before the
  name cache; write and search permission on a directory to make or
  remove a name; sticky directories (only the file's or directory's
  owner, or the superuser, may remove or rename a name: `EPERM`); and
  write permission on a directory moved to another parent.
- **New files** belong to the effective user and group, or to the
  directory's group if it is set-group-ID, in which case a new
  directory inherits the bit. A file `open` has just made is not
  checked against its own mode (so `O_CREAT|O_RDWR` with 0444 works);
  `open` always creates exclusively underneath, so a file someone else
  made meanwhile is checked as usual.
- **Changing attributes:** only the owner or the superuser may
  `chmod`; anyone else loses `S_ISVTX` on a file that is not a
  directory and `S_ISGID` for a group it is not in. `chown` follows
  `_POSIX_CHOWN_RESTRICTED`: the owner may change the group to one of
  its own, nothing more, and a change by anyone but the superuser
  clears the set-ID bits. A write by anyone but the superuser clears
  them too (System V).
- **`access` and `faccessat`** check with a copy of the credentials
  whose effective ids are the real ones, put in `l_cred` for the call,
  so the path's directories are searched as the real user too;
  `AT_EACCESS` uses the effective ids.
- **Set-ID programs:** exec gives the effective user or group of the
  file's owner or group for `S_ISUID` or `S_ISGID`, and then sets the
  saved ids to the effective ones. When the effective ids differ from
  the real ones the auxiliary vector carries `AT_SECURE` 1 (number 23,
  the C library's), and mlibc's dynamic linker ignores
  `LD_LIBRARY_PATH` and `LD_PRELOAD`.
- **Signals and limits:** a signal may be sent by the superuser, or by
  a process whose real or effective user id is the target's real or
  saved one, and `SIGCONT` to any process in the same session.
  `kill` to a group fails with `EPERM` if processes matched but none
  could be signalled. Only the superuser raises a hard resource limit.
- **The C library:** mlibc's `setuid`, `seteuid`, `setgid`,
  `setegid`, `getgroups`, and `setgroups` call the kernel. `truncate`
  is `open` for writing (which checks permission) and `ftruncate`.
- **Tests:** `credtest` (mlibc) starts as the superuser, sets files up
  on ext2, and checks each rule as user 100 in child processes,
  including a set-ID copy of itself that checks its ids and
  `AT_SECURE`. The process self-test now also counts `cred` among the
  zones that must not leak.
- **Not yet:** a login, `/etc/passwd`, and an init that drops
  privilege, so everything else still runs as the superuser; mount
  options (`nosuid`); `setreuid`; privileges finer than uid 0.

## Terminals (2026-11-13)

- **ABI:** System V's `termios` (four 32-bit mode words, as UnixWare's
  `tcflag_t` was 32 bits, and `NCCS` 19 control characters) and the
  older `termio` (16-bit words, a line byte, `NCC` 8), with System V's
  flag values, `c_cc` indexes (`VMIN` and `VTIME` share slots 4 and 5
  with `VEOF` and `VEOL`), and speeds (`CBAUD` 017, up to `B38400`).
  ioctls: `TCGETA`/`TCSETA`/`W`/`F`, `TCGETS`/`TCSETS`/`W`/`F`,
  `TCSBRK`, `TCXONC`, `TCFLSH`, `TIOCGWINSZ`/`TIOCSWINSZ`,
  `TIOCGPGRP`/`TIOCSPGRP`, `TIOCGSID`, `TIOCSTI`, and for any file
  `FIONBIO`, `FIOCLEX`, `FIONCLEX` (handled in `ioctl` itself) and
  `FIONREAD`. `TIOCSCTTY`, which UnixWare lacks, takes Solaris's
  number, `tIOC|132`. `tools/abicheck` checks all of them against
  mlibc's.
- **ioctl** reaches a vnode through a new, last `vop_ioctl`; file
  systems leave it NULL, which is `ENOTTY`.
- **Line discipline** (`kern/tty.c`, Belka's own): `c_iflag` input
  mapping and `IXON`/`IXANY`; `ISIG` sends `SIGINT`, `SIGQUIT`, or
  `SIGTSTP` to the foreground group and flushes input unless `NOFLSH`;
  canonical lines with `VERASE`, `VKILL`, and with `IEXTEN` `VWERASE`
  and `VLNEXT`, a read returning at most one line, `VEOF` ending a line
  unread (an empty one reads as end of file); otherwise `VMIN` and
  `VTIME` in the four POSIX combinations (`VTIME` from the read with
  `VMIN` 0, else from each character). Echo with `ECHOE`, `ECHOK`,
  `ECHOKE`, `ECHONL`, `ECHOCTL`; output with `OPOST`, `ONLCR`, `OCRNL`,
  `ONOCR`, `OLCUC`, and stopping. Modes leaving canonical mode pass on a
  half-edited line. `O_NONBLOCK` reads and writes give `EAGAIN`,
  `O_NDELAY` reads 0.
- **The console** is a terminal: its vnode routes read, write, ioctl
  and poll to it, output goes out raw under printf's lock (`cnwrite`),
  and the 16550 now takes input by interrupt (ISA IRQ 4, enabled in
  `configure()`) into it. Kernel messages still go to the device
  directly. Defaults: `ICRNL IXON IMAXBEL`, `OPOST ONLCR`, `CS8 CREAD`
  at `B38400`, `ISIG ICANON ECHO ECHOE ECHOK ECHOKE ECHOCTL IEXTEN`,
  and the usual control characters (`^C`, `^\`, DEL, `^U`, `^D`, `^Z`,
  `^S`/`^Q`, `^W`, `^V`), 24 by 80.
- **The C library:** Belka's ABI has its own `termios.h` and
  `ioctls.h`; mlibc's generic `termios.h` and `sys/ioctl.h` now define
  Linux's ioctl numbers only when the ABI has not. `tcgetattr`,
  `tcsetattr` (`TCSANOW`, `TCSADRAIN`, `TCSAFLUSH` pick the `TCSETS`
  form), `tcflush`, `tcdrain`, `tcflow`, `tcsendbreak` and the window
  size calls are the ioctls; `isatty` asks for `TCGETA`.
- **Tests:** `ttytest` (mlibc) types on the console with `TIOCSTI`, echo
  off, and checks lines and editing, end of file, the four `VMIN`/`VTIME`
  cases, `O_NONBLOCK`, the signal characters and `NOFLSH`, `SIGWINCH`,
  stopped output, `TCGETA`/`TCSETA`, and flushing. Serial input through
  the interrupt was checked by hand, feeding characters to QEMU's serial
  port; the automatic tests' serial port is a file, with no input.
- **Not yet:** fill and delay characters, and `VDISCARD`, `VREPRINT` and
  `VDSUSP`. (Job control and pseudo-terminals came on 2026-11-14,
  below.)

## Job control and pseudo-terminals (2026-11-14)

- **Controlling terminal:** a session leader with none that opens a
  terminal belonging to no session, without `O_NOCTTY`, makes it its
  own with its group in the foreground (SVR4), or takes it with
  `TIOCSCTTY`. The terminal records its session and foreground group
  (`t_sid`, `t_pgrp`); a list of every terminal finds a session's. The
  kernel's own session (0) never has one, so the processes the kernel
  starts are not subject to job control.
- **Background access** (POSIX): a read by a process of a background
  group of the terminal's session sends `SIGTTIN` to its group, and a
  write with `TOSTOP`, or `tcsetattr`, `tcflush`, `tcflow`, `tcdrain`,
  `tcsendbreak` and `tcsetpgrp`, sends `SIGTTOU`. The call then ends
  with `ERESTART`, so it is made again once the group continues (or
  `EINTR` for a handler without `SA_RESTART`). An orphaned group gets
  `EIO` instead, as does a read with `SIGTTIN` held or ignored; a write
  with `SIGTTOU` held or ignored goes on. `VSUSP` does not stop an
  orphaned group.
- **SIGHUP:** when a session leader exits, its terminal's foreground
  group gets `SIGHUP` and `SIGCONT` and the session loses the terminal.
  A hang-up (a pseudo-terminal's master closed) does the same for the
  foreground group and the leader's group, and from then on reads see
  end of file and writes fail with `EIO`.
- **Orphaned process groups** (POSIX): an exit that orphans a group
  with a stopped member, its own group or a child's (up to 32 distinct
  groups checked per exit), sends each member `SIGHUP` and `SIGCONT`.
  A group is orphaned when no live member has a parent in another group
  of the same session; process 0 is in session 0, so a group of another
  session whose members' parents are gone is orphaned.
- **ioctl numbers** for pseudo-terminals are System V's: `ISPTM`
  (`('P'<<8)|1`) and `UNLKPT` (`('P'<<8)|2`).
- **devfs**, mounted on `/dev` (once; until there are device files on
  disk): `console`, `null`, `zero`, `tty` (the caller's controlling
  terminal, opened in its own right; `ENXIO` if none), `ptmx`, and
  `pts/N`. Device numbers (`<sys/mkdev.h>`, System V's names) put the
  major in the high 32 bits; the majors are Belka's. Device vnodes share
  `spec_*` operations, with their owner and mode in a `struct specattr`.
  `vn_openat` lets `VOP_OPEN` put another vnode in its place, which
  `ptmx` and `tty` do.
- **Pseudo-terminals** (`kern/tty_pty.c`), after System V's ptm and pts
  (up to 64): opening `ptmx` makes a pair and opens its master; the
  slave, a full terminal with the line discipline, may be opened once
  `UNLKPT` was given, and belongs to the real user and group that opened
  the master, mode 0620, so `grantpt` has nothing to do. The slave's
  output goes into a 4 KB buffer the master reads (a full one makes the
  slave's writes wait, `t_outspace`); what the master writes is the
  slave's input, waiting for room. Closing the last master file hangs
  the slave up; with every slave file closed, the master reads end of
  file. ioctls on the master other than `ISPTM`, `UNLKPT` and
  `FIONREAD` are the slave terminal's (modes, window size). A pair goes
  when neither vnode is referenced, and its name leaves the name cache.
- **The C library:** `posix_openpt` opens `/dev/ptmx`; `unlockpt` is
  `UNLKPT`; `ptsname` checks `ISPTM` and makes `/dev/pts/N` from the
  master's minor number.
- **Tests:** `ptytest` (mlibc) checks devfs, a pair both ways with echo,
  raw mode and the window size, end of file on either side, a session
  leader taking its controlling terminal on open, `/dev/tty`, `SIGTTIN`,
  `SIGTTOU` with `TOSTOP`, `EIO` with `SIGTTIN` ignored, `^C` through
  the master, `SIGHUP` on hang-up and on the leader's exit, and `SIGHUP`
  with `SIGCONT` for an orphaned group with a stopped member. With the
  stop signals disabled in the kernel it fails, as it should.
- **Not yet:** `SIGHUP` on a real line's carrier loss (no modem
  control), packet mode, BSD-style `/dev/ptyXX`, and an init and login
  that set up sessions on terminals.

## Processor time (2026-11-14)

- **Sampled at the clock tick,** in the System V manner: each tick is
  charged to the thread it interrupts, as user time if the interrupted
  code was in user mode (`%cs` at privilege 3) and as system time
  otherwise, interrupt handling included. A thread keeps `kt_ticks` and
  `kt_uticks`; a process keeps the same for its exited LWPs and for its
  reaped children. The resolution is the tick (10 ms); precise
  accounting at kernel entry and exit was not needed yet.
- **Reported by** `times`, `getrusage`, `_lwp_info`, and, for an exited
  child, the `si_utime` and `si_stime` of the `SIGCHLD` and `waitsys`
  `siginfo_t` (clock ticks; the child's own time with that of the
  children it reaped). The C library's `wait3` and `wait4` fill
  `ru_utime` and `ru_stime` from there; the other `rusage` fields are
  zero. Stopped and continued children report no time.
- **The CPU limit and the CPU-time clocks** count user and system time
  together.
- **Tests:** `libctest` spins (user time) and copies from `/dev/zero`
  (system time), and checks each lands on its side in `times`,
  `getrusage` and `wait4`. Counting every tick as user time makes it
  fail.

## Interval timers (2026-11-14)

- **`setitimer` and `getitimer`** are UnixWare's 169 and 170; `alarm`
  (27) is the real timer without an interval. Each process has three,
  in clock ticks (`kern_itimer.c`): `ITIMER_REAL`, a callout, sends
  `SIGALRM`; `ITIMER_VIRTUAL` counts down at each tick taken in user
  mode (`SIGVTALRM`), `ITIMER_PROF` at every tick of the process's
  (`SIGPROF`), both from `sched_tick` on any processor by
  compare-and-swap. An expired timer with an interval is reloaded.
- **The real timer's state** is under `p_siglock`, which its callout
  takes: cancelling clears `p_alarm` first, so a callout already on its
  way does nothing, and `untimeout` waits for one that is running. Its
  callout is set a tick past the expiry recorded, since a callout runs
  at a tick boundary that may come at once; the time left reads as
  asked, and never less than that passes.
- **Not inherited** by `fork`; kept across `exec`; the real one is
  cancelled at exit. mlibc's `setitimer`, `getitimer` and `alarm` are
  the calls themselves.
- **Tests:** `libctest`'s `test_itimers`: a repeating real timer, alarm
  sharing it, the virtual timer ignoring sleep and system time, the
  profiling timer counting system time, old values, and a child without
  timers. Counting system time against the virtual timer makes it fail.

## Init (decided and built 2026-11-14)

- **BSD-style init and rc,** not System V's `inittab` and run levels:
  NetBSD's `init` (`contrib/netbsd`, BSD licences kept with the code),
  `/etc/rc` with `/etc/rc.conf` over `/etc/defaults/rc.conf`, single
  user and multi user only. The rc is Belka's own and minimal for now:
  no `rc.d` or `rcorder` until there are utilities and services for
  them; site additions go in `/etc/rc.local`.
- **The boot:** without `selftest` on the command line (the test ISOs
  have it), the kernel mounts the ext2 root from `root=vdN` (`vd0` by
  default), devfs on `/dev`, and runs `/sbin/init` (or `init=path`;
  `single` passes `-s`) as process 1. Init starts in process 0's session
  and makes its own with `setsid`; it never gets a controlling terminal
  (`tty_open`). If it exits, the kernel panics.
- **Orphans go to init** (`initproc`), which hears of zombies among
  them by `SIGCHLD`; until init runs, and in a self-test boot, process 0
  reaps them as before. `kill(-1, sig)` leaves init alone, as System V
  does (the caller is still included).
- **`uadmin`** (UnixWare's 55, `kern_uadmin.c`), for the superuser:
  `A_REBOOT` stops at once; `A_SHUTDOWN` sends `SIGKILL` to every other
  process but init, waits up to five seconds, writes back every file
  system with `vfs_shutting_down` set, so ext2 also marks itself clean
  as an unmount would, and then halts or reboots. Belka's
  `reboot`/`halt` first stop init's logins (`SIGTSTP`), send `SIGTERM`
  to all, wait two seconds and `sync`.
- **Logins:** init runs getty on each line `/etc/ttys` turns on. getty
  is Belka's own and small (no `gettytab`): it takes the line as its
  controlling terminal, sets sane modes, and runs NetBSD's `login`
  (set-user-ID root), which checks the password with `crypt` and drops
  to the user's IDs. Root may log in only on a line marked `secure`.
  The password file is `/etc/passwd` itself for now (mlibc reads no
  shadow file), with NetBSD's `$sha1$` hashes: bcrypt is left out for
  its advertising clause. The image's root has no password.
- **The shell** is mksh R59c (`contrib/mksh`), `/bin/sh`, built without
  its `Build.sh`: a hand-written configuration and our own generators
  for its option tables and signal names.
- **The compat library** (`usr/lib/compat`, MIT) gives the imported
  programs the BSD interfaces mlibc lacks: `<sys/cdefs.h>` macros, an
  in-memory `dbopen` for init's session table, `getttyent`, `fparseln`,
  `strvis`, `crypt`, `getprogname`, `strtonum`, `sysctl` for
  `hw.machine`. Some are stand-ins: `setlogin` and `revoke` do nothing
  (the kernel keeps no login name and cannot revoke a terminal yet),
  utmp and wtmp are not kept, and `arc4random` is not for secrets.
- **mlibc fixes found on the way:** `login_tty` closed its descriptor
  even when it was 0 to 2; reading a line-buffered or unbuffered stream
  now flushes line-buffered output first, so a `printf` prompt shows
  before `getchar` waits; `strsignal` knows `SIGEMT`, `SIGWAITING`,
  `SIGLWP` and `SIGAIO`; the Belka ABI's `NSIG_MAX` is 35, not Linux's
  1024; and `socket` fails with `EAFNOSUPPORT`, which keeps `syslog`
  quiet.
- **Images and tests:** `tools/mkroot.sh` builds the root image with
  `fakeroot` (root-owned files, set-user-ID `login`). `make run` boots
  it to a login prompt on the serial console; `make run-selftest` is the
  old self-test boot. `make test-boot` boots the test image, whose
  `/etc/rc.local` runs `usr/tests/boottest` in the background: it checks
  root under init with devfs, an orphan going to init, and on a
  pseudo-terminal a wrong password turned away, root refused on a line
  not marked secure, and the test user logged in to a shell with the
  user's IDs, home and name; then it halts. The run must show that and
  the console's getty, and the image must pass `e2fsck` afterwards.
- **Not yet:** `rc.d`, a shadow password file, utmp, `revoke`,
  `setlogin`, setting the host name. (The utilities came the same day;
  see below.)

## Utilities (2026-11-14)

- **ksenon-utils,** the Ksenon system's own 45 System V-style
  utilities (C99, POSIX-2001/XSI, MIT), are `/bin`'s: the
  `contrib/ksenon-utils` submodule, on that repository's `belka` branch,
  which adds `lib/plat_belka.c` (the mount table and device numbers).
  They build with their own strict flags and their own `libxn`, against
  mlibc alone, not the compat library. As their own Makefile does, all
  but `mv` (and so `cp` and `ln`) are linked dynamically against
  `libc.so.1`; `mv` is static, so that it runs while the libraries are
  being moved. Their suite needs `grep`, `sed` and `awk`, so it runs on
  GNU/Linux; on Belka, `usr/tests/utiltest.sh` (only the utilities and
  the shell) runs from the boot test.
- **Calls they brought:** `statvfs` and `fstatvfs` (UnixWare's 103 and
  104; `<sys/statvfs.h>`, System V's struct with `f_basetype`, its
  fields 64 bits wide), `utime` (30) and `utimensat` (Belka's 227:
  nanoseconds, `UTIME_NOW`, `UTIME_OMIT`, `AT_SYMLINK_NOFOLLOW`; the
  owner or the superuser sets any times, anyone who may write sets them
  to now), `nice` (34) and `stime` (25).
- **Nice values** are System V's, 0 to 39 with `NZERO` 20, per process
  and inherited by `fork`. A process's LWPs share one time-share job,
  whose weight index is 40 less the nice value, so the default stays at
  index 20 and each step is about ten percent of the processor. Only the
  superuser lowers it; `nice()` returns the new value less `NZERO`.
- **The mount table is the kernel's,** as Solaris made it: devfs serves
  `/dev/mnttab`, made afresh from the mount list on each read, in System
  V's format (special, mount point, type, options, time, by tabs), and
  `/etc/mnttab` on the image is a symbolic link to it. Each `struct vfs`
  records its device (ext2 names it; others give their type), mount
  point and time, and `VFS_RDONLY`, which `statvfs` reports as
  `ST_RDONLY`. The console now has a device number of its own (major 1).
- **mlibc** has the sysdeps, and `ttyname` from the device number
  (`/dev/console`, `/dev/pts/N`), which `tty` uses and which login needs
  to see that the console is marked secure; `clock_settime` sets the
  time through `stime`, to the nearest second.
- **The link-editor read its arguments out of order** on a glibc host:
  glibc's `getopt()` permutes `argv`, so positional options (`-z
  allextract`, `-B static`) came to apply to nothing. Its option string
  now begins with `+`. That had left mlibc's regex functions out of
  `libc.so.1`, which the utilities found.
- **Tests:** `utiltest.sh` checks text tools, files, links, modes and
  owners, times set by `touch -t`, the mount table and `df`, device
  numbers in `ls -l`, `tty`, `nice`, and more. Making `utimensat` ignore
  the modification time makes it fail.

## Terminal descriptions (decided 2026-11-14)

- **terminfo, not termcap,** when curses and full-screen programs come:
  System V's compiled database, as SVR4 and UnixWare had and as curses
  expects. Belka's getty needs neither.

## File systems

1. **tmpfs**, for testing.
2. **ext2**, the first practical file system: a fresh MIT implementation
   modelled on NetBSD's `ext2fs` (see above).
3. **XFS**, specified from the published XFS design documents: on-disk
   format version 4 with version 2 directories. The Linux implementation
   is not followed.

## ABI

- **Native ABI:** AMD64 System V psABI, with system calls entered through
  the `syscall` instruction. Results come back System V style: carry
  clear with two values in %rax and %rdx, or carry set with the errno in
  %rax (see User processes above).
- **System call numbers and errno values** follow UnixWare where
  practical. Belka's own calls, for what UnixWare 2 does not have, are
  numbered from 210, after its last (2026-11-10); two use numbers it
  left free (181 and 182, the address waits).
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

- **C library:** mlibc, with a Belka system-dependent layer (see The C
  library above).
- **Out of scope for now:** MAC, ACLs, auditing, and other security
  features. Their hooks are not designed in until they are scheduled.

## Open questions

- **NetBSD network drivers under DLPI.** Imported drivers expect `ifnet`.
  A shim from `ifnet` to DLPI is needed whichever socket interface is
  chosen.
