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
  utilities (C99, POSIX-2001/XSI, MIT), are `/bin`'s (superseded: `/usr/bin`; see
  [The file system hierarchy](#the-file-system-hierarchy-2026-11-23)): the
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

## The interactive console (2026-11-14)

- **`make test-console`** (in `test-all`) types at the serial console
  as a person would: `tools/condrive`, a host tool, follows a script of
  `expect`, `send` and `sleep` lines (`usr/tests/console.script`) over
  QEMU's Unix socket. It logs in as root on the console (marked secure
  in `/etc/ttys`, which login can now see through `ttyname`), checks
  `/etc/profile`'s `PATH`, recalls history with `^P`, stops a command
  with `^C`, stops and continues one with `^Z` and `fg`, runs `ps`, logs
  out and in through a fresh getty, and halts, leaving the image clean.
  mksh's `sleep` is built in and runs in the shell, so the script uses
  `/bin/sleep` for a job to stop.
- **`/etc/profile`** sets `PATH` (`/sbin` and `/usr/sbin` for root),
  `TERM` and the umask. **`make run`** first repairs the root image with
  the host's `e2fsck -p`: a boot not ended by `halt` leaves it dirty,
  which the kernel mounts read-only, and there is no fsck on Belka yet.

## /proc and ps (2026-11-14)

- **IRIX's /proc,** System V Release 4.0's flat file system, not SVR4.2's
  directories (UnixWare's) nor Linux's or BSD's: `/proc/NNNNN` for each
  process (decimal, five digits or more), owned by its real user, mode
  0600, asked about with `ioctl()`; and IRIX's `/proc/pinfo/NNNNN`, mode
  0444, which answers only `PIOCPSINFO`, so that `ps` needs no
  privilege. `<sys/procfs.h>` has SVR4's ioctl number and field names in
  Belka's 64-bit layout (with `pr_euid` and `pr_egid` added, and IRIX's
  `pr_ctime`). The kernel mounts it on `/proc` at boot.
- **Only `PIOCPSINFO` so far:** the state letter (O, R, S, T, Z), ids,
  nice value, priority and class, address-space size, processor time,
  start time, controlling terminal, command name and arguments (kept at
  exec, `p_psargs`). Reading a process's address space and the
  debugging ioctls are for later; `read()` gives `EIO`.
- **Nodes:** the two directories are static; a process's file is a
  private vnode made at each lookup, holding only the process id, and
  freed (after leaving the name cache) when unreferenced, so a file
  whose process has gone answers `ENOENT`. `p_as` is now replaced under
  `proctree_lock` (exec, exit), so `/proc` can measure another
  process's address space.
- **`ps`** (`usr/bin/ps`, Belka's own) is System V's: `-e`/`-A`, `-a`,
  `-d`, `-f`, `-l`, `-p`, `-t` and `-u`, with its column layouts, over
  `/proc/pinfo`.
- **Tests:** boottest checks owners and modes, the listing, init and
  itself, a stopped child (T), a zombie (Z), and that a non-root
  process is refused init's file but not its pinfo; utiltest and the
  console session run `ps`. Reporting no stopped state makes boottest
  fail.

## Terminal descriptions (decided 2026-11-14)

- **terminfo, not termcap,** when curses and full-screen programs come:
  System V's compiled database, as SVR4 and UnixWare had and as curses
  expects. Belka's getty needs neither.
- **NetBSD's** (built 2026-11-14), not ncurses: libterminfo (which also
  answers the termcap calls), tic, infocmp and tput, plain C with no
  configure step. Its compiled database is NetBSD's one constant
  database, `/usr/share/misc/terminfo.cdb`, not SVR4's tree of files.
  The descriptions are ncurses's source (MIT-style licence).
- **Compiled on Belka:** NetBSD generates libterminfo's `hash.c` with
  its scripts and nbperf, and builds the database and some built-in
  descriptions by running tic at build time. Belka runs nothing built
  from imported code on the host, so `tools/terminfo-hash.awk` makes
  `hash.c` (a plain search for the perfect hash), `termcap_hash.c` is
  written by hand, there are no built-in descriptions, and `/etc/rc`
  compiles the database at boot from the source on the image, when it
  is missing or older. That takes a second or two.
- `clear` is a script over `tput`. utiltest checks the database, tput,
  clear and infocmp.

## utmpx (2026-11-14)

- **The records are System V's,** `/var/adm/utmpx` and `/var/adm/wtmpx`
  (Belka's mlibc ABI has its own `<utmpx.h>`, musl's struct with these
  paths), kept through mlibc's POSIX functions.
- **Who writes them:** init (built with `SUPPORT_UTMPX`) writes the boot
  and down records and each getty's `LOGIN_PROCESS`, and marks a line's
  entry dead when its session ends; login writes the `USER_PROCESS`
  entry, through NetBSD libutil's `loginx`, `logoutx` and `logwtmpx` in
  the compat library. `who` lists the logins. Not kept: the remote
  address (there is no networking), NetBSD's lastlogx database, and the
  old utmp.
- **mlibc fixes:** `pututxline` now writes over the entry it names
  instead of always appending (else a logout left the login listed), and
  `getutxline` matches entries by their own type.
- **Tests:** boottest checks the boot record, the console's getty and a
  login in utmpx, the login in wtmpx, and `who`; the console session
  checks that after logging out and in there is one login on the
  console. Appending again leaves two.

## ed and passwd (2026-11-14)

- **ed** is NetBSD's, static in `/bin` so that it works when little else
  does, with its old command forms and without DES. (Superseded: it goes
  in `/sbin`; see
  [The file system hierarchy](#the-file-system-hierarchy-2026-11-23).)
- **passwd** is Belka's own (`usr/bin/passwd`), set-user-ID root: a user
  changes their own password after giving the old one, the superuser
  anyone's. The new one is asked twice, hashed with `$sha1$` under a new
  salt (8 characters, about 18,500 to 24,680 rounds), and written to
  `/etc/passwd` through `/etc/ptmp`, the lock, renamed over it. There was
  no shadow file yet, so the hashes were in `/etc/passwd` (until
  2026-11-16; see The shadow password file).
- **Tests:** in boottest's session the test user changes the password;
  then the old one is refused and the new one logs in. Writing the old
  hash back makes it fail.

## sed, grep, awk, curses and vi (2026-11-15)

- **sed, grep and awk** are NetBSD's: its sed, its grep (egrep and fgrep
  are links; no gzip, bzip2 or message catalogues), and the One True AWK
  as NetBSD keeps it, built from the parser its dist ships so that no
  yacc runs. All three are dynamic, in `/usr/bin`.
- **regex:** sed and grep need NetBSD's regex (`REG_STARTEND`, the GNU
  extensions behind `sed -g`), which mlibc's lacks. It goes in the
  compat library under names of its own (`compat_regcomp` and the rest),
  so that it never stands in for mlibc's, whose `regex_t` differs. ed
  now uses it too. fts(3) comes along for `grep -r`.
- **curses is NetBSD's libcurses**, the system's one curses, over the
  libterminfo and `terminfo.cdb` already there. ncurses was not taken:
  it brings a second terminfo reader and database format, and its build
  runs its own generator programs on the host. Two of libcurses's files
  (`inchstr.c`, `instr.c`) have a licence with an advertising clause and
  were left out; Belka's `instr.c` supplies those functions over the
  public interface.
- **curses and stream orientation:** curses writes control strings to
  its stream as bytes and characters with `putwc`. C fixes a stream's
  orientation at its first write and mlibc enforces it, where NetBSD's
  stdio lets the two mix, so curses (patched, `BELKA_BYTE_PUTWC`)
  converts each character with `wcrtomb` and writes bytes.
- **vi is nvi 1.81.6**, as NetBSD keeps it, with wide characters, its
  own regex, and its lines in db(3)'s recno; ex and view are links.
  NetBSD's db(3) (btree, hash, recno, mpool) replaces the compat
  library's small in-memory table. nvi's configuration is NetBSD's but
  for iconv (mlibc has none). It locks the files it edits with flock
  (see Record locks). Recovery files go in `/var/tmp/vi.recover`.
- **Generated files:** nvi's numbered tables (options, ex commands, ipc
  messages) are made by `tools/nvi-defs.awk` and its prototypes by sed,
  libcurses's `fileio.h` is kept by hand; none of the imported
  generators runs on the host.
- **The console's attributes:** its vnode answered with a fixed owner
  and mode and refused changes, so neither login (which gives the
  terminal to whoever logs in) nor vi's `mesg` could change them. It now
  keeps them as a device node does, and checks access by them.
- **Tests:** utiltest checks sed, grep, awk and ex in batch mode on
  Belka; the console test edits a file with vi (insert, delete and put,
  substitute, search, undo, write).

## Record locks (2026-11-15)

- **fcntl's record locks** (`F_GETLK`, `F_SETLK`, `F_SETLKW`), with
  System V's numbers, in `kern/vfs_reclock.c`: shared and exclusive
  locks on byte ranges, which may run past the end of the file. They
  are advisory.
- **Owned by the process:** its locks on a file are one set whatever
  descriptor it uses, replaced range by range (a lock that sticks out is
  split; touching ones of a type are joined). They are not inherited
  across fork, and all of them go when the process closes any
  descriptor for the file, and so at exit.
- **Waiting:** `F_SETLKW` sleeps until the range is free, ends with
  `EINTR` (or a restart) for a signal, and fails with `EDEADLK` when the
  wait would close a circle of waiting owners. Each waiting thread (not
  process: two LWPs of one process may wait at once) puts the owner it
  acts for and the one it waits for on a list of waiters, which the
  check follows.
- **flock (2026-11-16)** is BSD's, a call of its own (228): a whole-file
  lock owned by the open file, so dups and a fork's child share it, and
  it goes with `LOCK_UN` or the file's last close. A change is made as
  BSD makes it, by giving the lock up first, so two holders of a shared
  lock asking for an exclusive one do not wait for each other; flock's
  waits are not checked for deadlock, as in BSD. flock and fcntl locks
  meet in one list and refuse each other, as any two owners' locks do;
  `F_GETLK` reports a flock lock with `l_pid` -1.
- **A limit:** at most `reclock_max` (8192) locks in all; a request that
  needs more fails with `ENOLCK`, rather than letting a process exhaust
  kernel memory (a KM_SLEEP allocation that cannot be met panics). An
  unlock that splits nothing needs no new record, so it always works.
- **Structure:** each vnode has a list of its locks (`v_locks`), and one
  mutex covers them all, with one synchronisation variable that is
  broadcast whenever a lock goes or weakens. Per-vnode locking can come
  if contention shows. A lock set while another LWP closed the
  descriptor is dropped again, as the close would have taken it.
- **struct flock** is Belka's native layout, the C library's: no
  `l_sysid`, which only remote locks use. Remote locks are postponed.
- **Not done:** mandatory locking (System V's set-group-ID files whose
  reads and writes honour the locks) is left out for now.
- **Tests:** libctest on ext2 and tmpfs (conflicts, splitting, ranges,
  close and exit, waiting, `EDEADLK`, `EINTR`, lockf; flock's sharing by
  dup and fork, its other open files, waits, meeting fcntl; `ENOLCK`);
  utiltest checks that a second ex cannot write a file the first has
  locked.

## The shadow password file (2026-11-16)

- **/etc/shadow** holds the hashes, in System V's nine fields (name,
  hash, day of the last change, then ageing fields left empty), root's
  and mode 0400. `/etc/passwd` says `x`. The hashing is unchanged
  (`$sha1$`); a stronger scheme waits for PAM.
- **Readers:** login and init (NetBSD's, patched under `BELKA_SHADOW`)
  take the hash through `compat_shadow`, which puts it in place of the
  `x` in mlibc's getpwnam entry. That library allocates each field and
  frees it at its next call, so the hash is handed over the same way.
- **passwd** reads and writes `/etc/shadow` and records the day of the
  change. It holds the password files' lock while it writes:
  `lckpwdf()`, now in mlibc, a write lock on `/etc/.pwd.lock` tried each
  second for up to 15 seconds, as System V has it. It writes
  `/etc/stmp`, mode 0400, and renames it over `/etc/shadow`.
- **Ageing (2026-11-16)** is System V's, from the shadow fields (login,
  patched under `BELKA_SHADOW`, and `compat_shadow_age`): an account
  past its expiry day is refused, and warned of before it; a last change
  of 0 (`passwd -f`) or a password older than `max` days makes the user
  choose a new one before the login goes on, through login's own path,
  which runs passwd once unprivileged, rather than NetBSD's outright
  refusal; one expired more than `inact` days ago is refused; one within
  `warn` days of its end is warned of. passwd keeps users to `min` days
  between changes, and gives the superuser `-n`, `-x`, `-w`, `-f`, `-l`
  (`*LK*`) and `-d`, and anyone `-s`, the state in System V's form.
- **crypt's failure** is libxcrypt's `*0` (or `*1`), not NULL: login and
  su compare its answer without looking for NULL, so a locked account
  crashed login.
- **su** is NetBSD's, without PAM: the hash from `/etc/shadow`; only
  members of `wheel` may become root.
- **lastlogx:** the "Last login" line. NetBSD's `getlastlogx` and
  `updlastlogx` are Belka's own in the compat library: a db(3) hash by
  uid in `/var/adm/lastlogx`, under `flock` locks.
- **Tests:** boottest: the user cannot read `/etc/shadow`; after passwd
  it is still root's and 0400, with a new hash and today's date; the old
  password fails and the new one works, and that login names the last.
  On test3: the change `passwd -f` makes due is asked for at login, the
  warning, `min`, an expired account and a locked one. su: refused to
  root outside `wheel`, test2 with its password, not without.

## Utilities, mount and rc.d (2026-11-16)

- **ksenon-utils' own suite runs on Belka** (boottest, over a directory
  of just the 45 utilities): 525 checks, all passing. Its harness was
  made to run on BSD-style systems too (still 554 of 554 on GNU/Linux):
  file times through BSD's `stat -f` where there is no GNU `stat -c`,
  a deep tree built and read a directory at a time, and none of GNU's
  `head -c` and bare `nice`.
- **What it found, in mlibc:** `getopt()` permuted operands as glibc's
  does, so NetBSD's find took its expression for paths; it now stops at
  the first operand on Belka (`MLIBC_POSIX_GETOPT`; `getopt_long` keeps
  GNU's permutation). `getopt` also kept its place in a word after a
  cluster ended it (`grep -cv -e PAT` failed), and refused an
  option-argument beginning with `-`. `mktime` and `timegm` did not
  normalise a `struct tm`, so touch took a 13th month.
- **New tools:** NetBSD's mktemp, seq, stat (readlink), find, xargs,
  getconf and hostname. find's `-fstype local` takes every Belka file
  system as local (a two-line patch). Their BSD needs are compat stand-
  ins: `st_flags` reads a spare stat field (Belka has no file flags),
  `major` and `minor` in Belka's encoding (not mlibc's Linux one), a
  small `parsedate` of Belka's own, `sys_signame`, `strmode`,
  `setmode` (NetBSD's) and the rest.
- **mount(2) and umount(2)** are System V's (21, 22): `MS_DATA` with a
  type name; ext2fs on a disk's block device, tmpfs on any name, and
  `MS_RDONLY`; the superuser's. devfs has a block node per disk
  (`/dev/vd0`, major 6), not yet readable. The mount table names a disk
  by its node. `mount` and `umount` are Belka's own, System V's, over
  `/etc/vfstab`; `/tmp` is a tmpfs, 1777. Not yet: remounting, nosuid,
  file system options.
- **rc.d:** NetBSD's `rc.subr` and its marker scripts, Belka's own
  `rcorder` (NetBSD's has a four-clause licence with an advertising
  clause), and `/etc/rc` as a small driver of its own: NetBSD's `rc`
  needs NetBSD's `sh` (`fdflags`) for its boot log. Belka's scripts:
  `mountall` (provides NetBSD's mountcrit stages), `terminfo`,
  `hostname` (rc.conf's `hostname` or `/etc/myname`). There is no
  `rc.shutdown` yet: halt and reboot go straight to uadmin.
- **sysinfo(2)** (139, System V's) reports the uname names and lets the
  superuser set the host name, which `sethostname` uses.

## Signal values and waits (2026-11-17)

- `sigqueue` (229, a Belka number) sends a signal with an application
  value to one positive PID, using `kill`'s permission checks. Signal
  zero checks access without sending anything. The first pending
  standard signal keeps its `siginfo`, including any `SI_QUEUE` value.
  Repeated signals coalesce under the existing posting rule.
- mlibc exposes `sigqueue`, `sigtimedwait`, `sigwaitinfo` and `sigwait`.
  The synchronous waits use the existing `sigwait` syscall (194).
  `sigwait` returns an error number directly and resumes after `EINTR`.

## Delayed terminal suspend (2026-11-17)

- POSIX.1-2001 reserves `VDSUSP` as an XSI extension. Belka follows
  Solaris `termio(4I)` for timing: with `ISIG` and `IEXTEN` set, the
  input queue marks the character and a foreground read posts `SIGTSTP`
  when it reaches the mark. The mark is consumed, and a read returns
  any preceding bytes before processing the remaining input on the
  next read. `VLNEXT` quotes the character as usual.
- [POSIX.1-2001 `<termios.h>`](https://www.gaertner.de/~neitzel/susv3/basedefs/termios.h.html)
  and [Solaris `termio(4I)`](https://docs.oracle.com/cd/E88353_01/html/E37851/termio-4i.html).

## Serial carrier loss (2026-11-17)

- The 16550 console handles a falling data-carrier-detect interrupt.
  With `CLOCAL` clear, the terminal sends `SIGHUP` to the controlling
  process. Reads see EOF, writes fail with `EIO`, and `poll` reports
  `POLLHUP`. The next open after the last console file closes clears
  the hangup. The built-in console defaults to `CLOCAL` for direct
  serial connections, and getty preserves it.
- Connection setup does not wait for carrier, and closing the console
  does not lower DTR. These are separate modem-control operations.
- [POSIX.1-2001 `<termios.h>`](https://www.gaertner.de/~neitzel/susv3/basedefs/termios.h.html)
  and [Solaris `termio(4I)`](https://docs.oracle.com/cd/E88353_01/html/E37851/termio-4i.html).

## Asynchronous and real-time signal options (2026-11-17)

The real-time option is superseded by [Real-time signals](#real-time-signals-2026-11-17).

- Belka reports `_POSIX_ASYNCHRONOUS_IO` and `_POSIX_REALTIME_SIGNALS`
  as unsupported in `<unistd.h>` and through `sysconf`. Both are
  options in [POSIX.1-2001 `<unistd.h>`](https://www.gaertner.de/~neitzel/susv3/basedefs/unistd.h.html).
  `sigqueue` for the existing standard signals remains available.
- POSIX AIO completion with `SIGEV_SIGNAL` posts the signal selected
  by the application in `aio_sigevent`. STREAMS readiness via
  `I_SETSIG` and `SIGPOLL` is a separate interface.
  [POSIX.1-2001 signal generation](https://www.gaertner.de/~neitzel/susv3/functions/xsh_chap02_04.html),
  [Solaris STREAMS Programming Guide](https://docs.oracle.com/cd/E53394_01/html/E54835/appmech3-30379.html).

## STREAMS signal registration (2026-11-17)

- The initial STREAMS endpoint is `/dev/strloop`. Each open creates a
  private stream and bounded loopback queue. `read`, `write` and `poll`
  exercise band-zero data and flow control through the normal file API.
  Modules, priority band queues and `getmsg`/`putmsg` remain to be added.
- `I_SETSIG` and `I_GETSIG` use the System V ioctl and mask values.
  Registration is per process and stream. A forked process can register
  on the inherited descriptor independently. Closing a descriptor for
  the stream removes the closing process's registration, including
  when another descriptor still refers to the stream. Ordinary data
  arriving at the head and a full queue becoming writable generate
  `SIGPOLL` with `POLL_IN` and `POLL_OUT` respectively and band zero.
- [POSIX.1-2001 `<stropts.h>`](https://www.gaertner.de/~neitzel/susv3/basedefs/stropts.h.html),
  [Solaris `close(2)`](https://docs.oracle.com/cd/E19683-01/816-5213/6mbcef4an/index.html),
  [Solaris STREAMS signal events](https://docs.oracle.com/cd/E53394_01/html/E54835/appmech3-30379.html).

## Urgent data on local sockets (2026-11-17)

- The first socket endpoint is an unnamed, connected `AF_UNIX`
  `SOCK_STREAM` pair. Its receive queues support ordinary byte I/O,
  `send` and `recv`, and one out-of-band byte at a time. The mark
  separates ordinary bytes queued before and after the urgent byte.
  `sockatmark` and `POLLPRI` expose this state.
- `F_SETOWN` selects a process or process group and `F_GETOWN`
  returns the selection. Urgent arrival posts `SIGURG` to eligible
  recipients using the registration credentials. Process and group
  identities prevent an old registration from signaling a recycled ID.
- This local transport uses Belka's existing socket ABI constants.
  Named sockets, STREAMS TPI providers, ancillary messages and
  `SO_OOBINLINE` remain to be added. (`sendmsg` and `recvmsg`, without
  ancillary data: see [DHCP](#dhcp-2026-11-23).)
- [POSIX.1-2001 socket behavior](https://www.gaertner.de/~neitzel/susv3/functions/xsh_chap02_10.html),
  [descriptor ownership](https://www.gaertner.de/~neitzel/susv3/basedefs/fcntl.h.html),
  and [socketpair](https://www.gaertner.de/~neitzel/susv3/functions/socketpair.html).

## Pathname sockets and pipe creation (2026-11-17)

- Local stream sockets expose `socket`, `bind`, `listen`, `connect`,
  `accept`, and `accept4`. Binding creates a socket inode on tmpfs or
  ext2 and associates the listener with its vnode. Rename and unlink
  have normal filesystem semantics. Closing leaves the pathname in
  place and releases queued endpoint files. Accepted streams use the
  same data and urgent signal path as socket pairs.
- POSIX pipes and named FIFOs share fifofs. `pipe2` installs both
  descriptors with nonblocking and close-on-exec flags atomically.
- [POSIX.1-2001 local sockets](https://www.gaertner.de/~neitzel/susv3/functions/xsh_chap02_10.html)
  and [accept](https://www.gaertner.de/~neitzel/susv3/functions/accept.html).

## Regular-file AIO completion (2026-11-17)

- mlibc queues `aio_read`, `aio_write`, and `aio_fsync` for a
  per-process worker and exposes completion through `aio_error`,
  `aio_return`, and `aio_suspend`. A private close-on-exec reference
  protects the open file from descriptor reuse without releasing the
  application's record locks when the worker closes its reference.
  Fork copies neither requests nor their private references.
- `SIGEV_SIGNAL` posts the requested signal with `SI_ASYNCIO` and the
  supplied value, once the result is available. `SIGEV_NONE` leaves
  the result available without posting a signal. Standard signals
  still coalesce. Cancellation, list I/O, and `SIGEV_THREAD` remain
  open, so the complete POSIX AIO option is still unavailable.
  Queued delivery is extended by [Real-time signals](#real-time-signals-2026-11-17).
- [POSIX.1-2001 asynchronous reads](https://www.gaertner.de/~neitzel/susv3/functions/aio_read.html)
  and [signal generation](https://www.gaertner.de/~neitzel/susv3/functions/xsh_chap02_04.html).

## Real-time signals (2026-11-17)

- The kernel and libc expose signals 35 through 64 and up to 32 queued
  records per receiving process. An exhausted queue returns `EAGAIN`
  to the sender. Masks, waits, and handlers cover the entire range.
- Pending real-time signals are selected by lowest signal number. Each
  number is consumed in arrival order among signals eligible for the
  calling LWP. Pending traditional signals keep their previous priority.
- Traditional signals coalesce for ordinary posts. Value-bearing
  `sigqueue` and AIO notifications use the queue when the disposition
  has `SA_SIGINFO`. The AIO worker publishes the result before posting
  the notification and retries if the receiving queue is full.
- [POSIX.1-2001 signal generation and delivery](https://www.gaertner.de/~neitzel/susv3/functions/xsh_chap02_04.html).

## tcsh interactive shell (2026-11-17)

- tcsh 6.24.16 is imported from the upstream release with its notices
  retained. Its archive checksum and the local patch are recorded in
  `contrib/tcsh/README.belka`.
- The GNU make build cross-compiles an 8-bit shell against mlibc and
  the NetBSD terminfo termcap interface. Target configuration lives in
  `usr/lib/compat/tcsh`. Generated declarations use the target
  preprocessor and host text tools.
- The root image installs `/bin/tcsh`, registers it in `/etc/shells`,
  and includes its licence notices in `/usr/share/licenses/tcsh`.
  mksh serves init, rc, and the initial login shell. Users can start
  tcsh explicitly.
- The console test covers version reporting, history recall, pathname
  completion, suspend, foreground resume, interruption, and returning
  to the login shell. tcsh selects `wait4` for child resource accounting.
- [Upstream tcsh release](https://astron.com/pub/tcsh/tcsh-6.24.16.tar.gz).

## Terminal settings utility (2026-11-17)

- NetBSD's BSD-licensed stty runs over Belka's System V termios.
  The port maps numerical baud rates to System V speed codes and uses
  tcflow for output start and stop. Tables expose the available flags
  and control characters.
- The gfmt1 save and restore path handles shared canonical and raw
  control-character slots and restores both speeds through cfsetispeed
  and cfsetospeed. The console test exercises echo, saved settings,
  window dimensions, and a baud-rate change.

## Changing login shells (2026-11-17)

- chsh is Belka's MIT-licensed utility over the System V text passwd
  file. Ordinary users authenticate against shadow, select a shell from
  /etc/shells, and may change their own account. The superuser may
  select another executable shell or account.
- It shares lckpwdf with passwd, rechecks the account under that lock,
  preserves other accounts and fields, ownership and mode, and replaces
  passwd with a flushed and synced temporary file in /etc.
- The PTY login test checks authentication failure, ownership restrictions,
  shell-list validation, a successful switch and restoration, and file
  permissions.

## Interactive pager (2026-11-17)

- NetBSD's less 458 is imported under its BSD-style Less License.
  The same executable provides more through its invocation name.
- Belka selects POSIX signal masking and the termcap interface supplied
  by NetBSD's terminfo library. The pager supports file viewing, pipes,
  forward and backward movement, and searching.
- The console test checks end and beginning jumps, a forward search,
  more-style paging, and terminal restoration on exit.

## Manual pages and mandoc (2026-11-17)

- mandoc 1.14.6 supplies man, apropos, whatis, and makewhatis from one
  executable. Its upstream ISC and BSD licences are retained. zlib 1.3.1
  supplies compressed-page input under the zlib licence. Release URLs
  and archive checksums are in the imports' README.belka files.
- The target configuration is in usr/lib/compat/mandoc. The formatter's
  wide-character writes are converted to bytes, because it shares stdout
  with byte-oriented output and mlibc enforces stream orientation.
- Installed pages live in /usr/share/man. The manpages rc.d script
  creates mandoc.db on the first boot. Administrators can rebuild it
  with makewhatis /usr/share/man after installing more pages.
- Boot tests cover man and mdoc formatting, UTF-8, indexed and unindexed
  lookup, and apropos and whatis searches. less is the default pager.

## Archive utilities (2026-11-17)

- NetBSD's pax supplies /bin/pax, /bin/tar, and /bin/cpio from one
  executable, preserving its BSD notices. The import uses the same
  netbsd-10 revision recorded in contrib/netbsd/README.belka.
- SMALL excludes mtree specification input and alternate account
  databases. Archive ownership uses the system passwd and group files.
  Compression options invoke external compression programs.
- The Belka adapters supply account-name lookups, default signal
  re-raising, and no-follow mode changes. Symlink modes have no
  representation on Belka; ordinary files are chmodded without
  following links. Terminal filename listings use the existing vis
  encoding through its single-character interface.
- Boot tests exercise tar, ustar pax, pax copy mode, cpio, payload
  integrity, hard links, symlinks, FIFOs, and malformed input.

## gzip compression (2026-11-17)

- NetBSD's two-clause BSD gzip supplies gzip, gunzip, gzcat, and zcat
  over the existing zlib. It uses the same netbsd-10 import revision.
  The gzip and legacy pack decoders are enabled. The optional bzip2,
  xz, lzip, and compress decoders are disabled.
- The port omits BSD file flags and the unavailable SIGINFO hook.
  mlibc maps its internal empty-path descriptor timestamp operation to
  Belka's existing null-path form. File modes and times survive gzip
  and gunzip.
- Boot tests cover streams, aliases, concatenated members, integrity
  checking, truncation, refusal to overwrite, metadata preservation,
  tar -z round trips, and compressed mandoc input.

## Source comparison and patches (2026-11-17)

- NetBSD's newer BSD diff and netbsd-10 patch provide /usr/bin/diff and
  /usr/bin/patch. The diff revision is recorded in its README.belka,
  because netbsd-10 used GNU diffutils. The individual BSD licences
  and notices, including Caldera's advertising clause, are retained.
- patch uses POSIX.1-2001 posix_madvise and derives directory-name
  lengths from d_name. It runs over the existing regex and compat
  library.
- Boot tests cover diff statuses, unified and recursive diffs, applying
  and reversing patches, dry runs, path stripping, rejected hunks,
  and preservation of an unmatched file.

## Local account administration (2026-11-17)

- Belka's MIT-licensed useradd, usermod, userdel, groupadd, groupmod, and
  groupdel maintain System V text passwd, shadow, and group files.
  NetBSD's user tools require master.passwd and pw_mkdb, so this utility
  uses Belka's existing account format.
- New users start with a locked password. The default primary group is
  users, home is /home/name (superseded: /usr/people/name; see
  [The file system hierarchy](#the-file-system-hierarchy-2026-11-23)),
  and shell is /bin/sh. Options cover explicit
  identifiers, primary and supplementary groups, comments, homes, shells,
  user/group renaming, and creation of home directories.
- User renaming updates shadow and supplementary memberships. Group GID
  changes update primary-group references. User deletion removes the
  account and memberships. Files and home contents are administered
  through the existing file utilities.
- The shared accounts helper takes the existing lckpwdf lock. It stages
  replacements and hard-linked backups for all three files in the
  root-only /etc/.accttxn directory on the same file system. A synced
  ready marker precedes publication; a committed marker follows the
  synced renames. Recovery rolls back an interrupted publication or
  cleans up a completed transaction. This is recoverable publication
  across the text files; readers can run between individual renames.
- The accounts rc.d script runs acctrecover before services. passwd and
  chsh also recover under the same lock before writing. Names and passwd
  records respect the current libc parser's limits; identifiers fit its
  signed integer representation, and supplementary groups respect the
  sysconf limit.
- Boot tests cover administration, locked passwords, permissions, name
  and identifier collisions, root-only access, concurrent writers,
  reconstructed interrupted and committed transactions, and recovery
  through chsh.

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

## Native development bootstrap (2026-11-18)

- Import NetBSD bmake, m4 and tsort, Berkeley yacc and flex. Install bmake
  explicitly, and GNU make 4.4.1 as gmake and make for Belka and GCC build
  compatibility. Preserve each upstream licence; GNU make remains GPL.
  Use the MIT/X11 install-sh for install. bmake has a small MIT sys.mk.
- GNU make uses fork and exec; posix_spawn remains disabled and unapproved.
  NetBSD m4 supplies macro processing for flex and illumos sources. Clear
  expected pipe-seek errors in flex so its cleanup closes the output pipe.
  mlibc freopen preserves the stream descriptor when dup2 is available.
- Cross-build the existing illumos link-editor and its ar, nm and sgsmsg
  for Belka. Keep the compatibility changes in the MIT port layer, with
  a separate CDDL ar change to create archive-replacement files atomically
  using mkstemp. ranlib delegates index rebuilding to ar -s.
- Install libc headers, start files, static libc, compiler builtins and
  lex support libraries. Include the linker sources and licence alongside
  a native relink recipe and its initial target objects and archives.
  Native relinking is a bootstrap stage, not full source self-hosting.
- GCC 14 or newer is the initial native compiler direction. A native
  assembler, GCC target/driver port, compiler dependencies, C++ runtime,
  muon and samurai remain before a full compiler/libc source bootstrap.
  PathScale is deferred until that compiler is ready. See
  [native development](../native-development.md) for the staged workflow.

## Native GCC target and assembler (2026-11-18)

This replaces the compiler/assembler status in [Native development
bootstrap](#native-development-bootstrap-2026-11-18).

- GCC 14.2.0 and GNU as 2.43 are optional bootstrap tools. Preserve their
  upstream licences and ship small target patches with pinned, verified
  release archives rather than importing complete generated source trees.
  Keep illumos ld as the linker, with driver specifications for its
  interpreter, shared-object, static and PIE options.
- Build separate host cross and native compilers for amd64 LP64. Supply
  static libgcc/libstdc++ and GMP/MPFR/MPC libraries and headers. Disable
  multilib, LTO, plugins and optional runtimes in the initial port.
  posix_spawn remains disabled; tools avoid sbrk-based heap-size probes.
- Combine GCC's exception-registration CRT with Belka's libc array
  boundaries without defining __dso_handle twice. Finalize the executable
  handle before unregistering its unwind information, and omit executable
  array markers from shared libraries. Keep unwind tables read-only to match mlibc and illumos link semantics.
- Support amd64 ELF 32-bit signed and unsigned dynamic relocations with
  width-correct writes and overflow checks. For text relocations,
  temporarily permit writes to non-writable load segments and restore
  their original permissions before initializers run.
- Physical-memory sysconf queries report an indeterminate value until a
  kernel interface exists. Do not invent a memory count for compiler
  heuristics. Full native GCC source bootstrap and native muon/samurai
  remain separate acceptance work; see [native development](../native-development.md).

## Pipe writer notification race (2026-11-19)

- A writer drops fn_lock to notify readers and pollers before waiting for
  room. Recheck both reader presence and room under fn_lock after those
  callbacks; a reader can drain the pipe and issue its wakeup during the
  unlocked interval. Enter the writer wait only while its predicate is
  still false. This fixes the lost wakeup exposed by extracting the native
  libc source archive through gzip and tar. The existing lock order and
  pipe ABI remain unchanged.
- Exercise sustained writes larger than the pipe capacity with a bounded
  user-mode regression, checking byte order, completion and child status.

## Native build tools and libc source kit (2026-11-19)

This replaces the muon/samurai status in [Native GCC target and
assembler](#native-gcc-target-and-assembler-2026-11-18).

- Import samurai 1.3 and muon 0.7.1 with upstream licences and source
  hashes. Use external samurai, also installed as ninja, rather than
  embedding another job runner in muon. Keep job launch on fork/exec;
  posix_spawn remains unapproved.
- Recognise Belka in muon's machine and GCC toolchain selection. Use
  illumos link-editor options for shared libraries, SONAMEs, search paths
  and archive groups. Do not add a GNU linker or network dependency.
- Provide an explicit offline mlibc source kit with its pinned subprojects
  and overlays. Record revisions and omit Git metadata. Build static and
  shared libraries into a separate sysroot and test with that sysroot
  before replacing installed libraries. Preserve the illumos option
  adapter for mlibc's explicit GNU version scripts, using POSIX nm output.
- Increase optional compiler images to 32768 inodes to accommodate source
  extraction and object files; retain the normal image's inode count.
  See [native development](../native-development.md) for commands.

## Existing mkdir targets (2026-11-19)

- Follow [POSIX.1-2001 mkdir](https://pubs.opengroup.org/onlinepubs/009695399/functions/mkdir.html)
  for existing directories, including root and a final dot: return EEXIST.
  When parent lookup rejects a non-creatable parent/name pair with EINVAL,
  mkdir resolves the whole target and reports existence or the lookup error.
  This replaces mkdir's earlier EINVAL result for root; parent lookup itself
  and the rename/removal rules are unchanged. GCC's libcody build exposed
  the discrepancy through mkdir -p ./.

## Executable text files and PATH search (2026-11-19)

- Follow the [POSIX.1-2001 exec interfaces](https://pubs.opengroup.org/onlinepubs/009695399/functions/exec.html):
  execlp and execvp interpret an executable text file through the shell when
  execve returns ENOEXEC. Keep execve's error unchanged. Empty components of
  PATH search the current directory, including leading and trailing ones.
- Select /bin/sh with the sh utility personality. Passing the caller's
  name as the interpreter's name can select mksh's restricted mode when
  that name starts with r; GCC's generated ranlib script exposed this.
  Preserve the script path, subsequent arguments and environment. Test
  both static and shared libc, including arguments containing spaces and
  empty arguments. No kernel executable-format or syscall ABI change is
  involved.

## Native GCC source bootstrap kit (2026-11-19)

This extends [Native GCC target and assembler](#native-gcc-target-and-assembler-2026-11-18)
with an offline source bootstrap workflow; acceptance requires completion
of the three-stage build and comparison.

- Package the patched GCC 14.2.0 tree and pinned GMP, MPFR and MPC sources
  separately from sources used by host cross-builds. Keep installation
  under a staging directory until its compiler has been validated. Do not
  replace the installed compiler during the bootstrap.
- Supply optional GNU binutils object utilities from the existing pinned
  release. Keep illumos ld, ar and nm as the defaults. Restrict GNU strip
  use to bootstrap objects and archives; preserve illumos-linked dynamic
  executables when building images.
- Have libgcc build and install Belka's CRT array-boundary objects in
  addition to GCC's exception-registration CRT. Use POSIX tail syntax in
  the assembler probe. Continue to disable posix_spawn and sbrk probes.
- Use an 8 GB optional source-kit image with 262144 inodes. Recommend
  4 GB guest memory and one build job: the x86 machine-description
  generator alone exceeded a gigabyte in a measured host run.

## Reopened stream indicators (2026-11-19)

- Follow [POSIX.1-2001 freopen](https://pubs.opengroup.org/onlinepubs/009695399/functions/freopen.html):
  clear the stream's error and EOF indicators on reopening, alongside its
  existing orientation and conversion-state reset. Retain the descriptor
  preservation established for native build tools.
- GCC fixincludes first consumes its filename list to EOF, then reopens
  stdin before reusing it for a header filter. A stale EOF indicator made
  that filter stop after one buffer and produce a truncated header. Cover
  reopening streams after both EOF and read errors in the static/shared
  libc regression suite.

## Native GCC runtime selection and acceptance (2026-11-19)

This completes the initial acceptance work described in
[Native GCC source bootstrap kit](#native-gcc-source-bootstrap-kit-2026-11-19).

- Keep /usr/lib64 in GCC's standard startup-file and library search list.
  Do not inject an early -L/usr/lib64 through LINK_SPEC: explicit user
  directories and the compiler's private runtime directories must retain
  their normal precedence. Test this by linking against a user archive
  that shadows a system library.
- Validate a staged compiler using its own cc1/cc1plus, libgcc, libstdc++
  and Belka CRT objects. Exercise static and dynamic C/C++, PIE, TLS,
  exceptions, threads and shared libraries before replacing system tools.
- GCC 14.2.0 completed three native stages and the stage-two/stage-three
  object comparison under QEMU TCG. The run started with 4 GB and one job
  and finished with 8 GB and two jobs. The library-search correction was
  applied afterward, the drivers rebuilt natively, and all 46 staged
  compiler/runtime checks passed. The full comparison has not been
  repeated with that driver-only change; the DejaGNU suite remains future
  verification. Keep these limits explicit in the native development guide.

## Shareable process resource ownership (2026-11-20)

- Address spaces carry an atomic ownership count. as_alloc supplies one
  reference, as_hold adds another, and as_free releases it. Only the last
  release destroys segments and page tables; all processors must have
  stopped using the address space before that final release.
- Move descriptor arrays and close-on-exec flags into a separately owned
  table with its own mutex. Process limits remain under p_lock; the table
  mutex precedes p_lock when checking a descriptor limit. File release and
  record-lock cleanup run after dropping the table mutex.
- An exiting owner releases its own record locks and STREAMS registration
  while preserving the shared table's file references for remaining
  owners. Exec first obtains a private table, then closes its marked
  descriptors. Fork still copies the table and omits private AIO references.
- These ownership rules prepare process resources for sproc creation.
  They replace the original assumption that a process exclusively owns
  its address space and descriptor arrays; they do not change syscall
  numbers or expose a share-group creation interface yet.

## Share-group creation and control (2026-11-20)

This extends [Shareable process resource ownership](#shareable-process-resource-ownership-2026-11-20)
with independently implemented sproc interfaces and replaces process-local
heap bookkeeping with address-space-owned heap state.

- Assign Belka extension syscall numbers 242–245 to sproc, sprocsp, prctl
  and procblk. Preserve the existing System V carry/error and two-register
  result convention. Public sharing flags and supported control option
  values follow the documented interface; this is not IRIX binary ABI
  compatibility. Unknown control options return EINVAL.
- Members are separate, waitable processes with independent signals and
  timers. A creator becomes a member with all sharing permissions. Child
  sharing can only narrow its parent's permissions. A process cannot mix
  sproc membership with the historical use of multiple LWPs until exec.
- Share address spaces and descriptor tables by ownership reference.
  Synchronize directory, umask, soft file-size limit and credential
  snapshots through group master versions at syscall and user-return
  boundaries. Credential synchronization copies full credentials for a
  privileged recipient and only effective uid/gid for other recipients.
  Serialize group membership under proctree_lock and its master state
  under sg_lock; keep filesystem operations outside these locks.
- Reserve automatic stacks top-down in the common address space and
  allocate their pages on demand. Use the stack limit by default, or
  64 MB for an unlimited limit. These are full virtual reservations,
  rather than incrementally grown stack regions. Retain the creator's
  original stack until the address space dies; release automatic member
  stacks on exit/exec. Caller-supplied stacks remain caller-owned.
- Keep heap extent and its mutex in the address space so shared brk calls
  operate on one extent. Ordinary fork makes a private copy of the entire
  shared layout, including stacks, without joining the group. Exec leaves
  the group, obtains a private descriptor table before close-on-exec, and
  installs a new address space. Surviving members continue after creator
  exit. Members share their creator's fair-share scheduling job.
- Implement counted process blocking, unblock credits, group operations,
  exit/abort notifications, parent-exit SIGHUP, unblock-on-exec/exit and
  group queries. Preserve range and permission checks without reproducing
  the reference's undefined success result for group blocking. Stack-limit
  control respects Belka's existing restriction on unprivileged hard-limit
  increases.
- On amd64, retain callee-saved registers and floating-point controls,
  reset floating-point registers, and enter with the normal C stack
  alignment. Use separate FS bases for libc TLS. An optional sixth raw
  creation argument supplies a private-data pointer and a one-byte
  retirement address; zero supplies neither. The kernel writes retirement
  only after the process can no longer return to the old user context.
- mlibc prepares a separate initialized TCB before making a child runnable,
  with a unique logical lock-owner id within the shared VM. Its runtime
  linker tracks allocation bases and retirement bytes; subsequent creation
  and successful exit waits reclaim retired TLS and local metadata. A
  child callback returns through exit(0); group exits release libc's exit
  guard so survivors can also exit. PR_NOLIBC skips fresh TLS allocation
  and leaves concurrent libc use to the caller. Raw test children use no
  libc TLS and call only the raw syscall helpers.
- This step does not add per-member mapping overlays/MAP_LOCAL, a fixed
  PRDA, address attachment, gang scheduling controls or checkpoint PID
  selection. Private-address-space creation has fork-like libc lock
  constraints when other members are concurrently using the runtime.
- Cover both static and shared libc. GNU strip was observed removing
  initialized TLS from an illumos-linked static executable's file-backed
  LOAD range. Link installed static tests again with the link-editor's -s
  option, preserving TLS and keeping the test images small.

## The network stack plan (2026-11-22)

- **Whose code:** Belka's own. IP, ARP, ICMP, UDP and TCP are written
  from the RFCs as STREAMS modules and drivers, under MIT, in the
  architecture fixed above (TCP/IP over DLPI, sockfs over TPI). Neither
  NetBSD's netinet nor lwIP is imported: both are built around their
  own buffers, interfaces and threading, and adapting them to queues
  and messages would rewrite most of them.
- **IPv4 first, IPv6-ready:** the first stack is IPv4 with ARP and
  ICMP. Transport modules, TPI addresses and sockfs take their address
  family from the message, not from fixed `sockaddr_in` layouts, so
  IPv6 (with neighbour discovery and ICMPv6) is added beside IPv4
  rather than through it.
- **Order:** (1) the STREAMS framework (messages, queues, put and
  service procedures, multiprocessor synchronisation, the stream head,
  `getmsg`/`putmsg`, module push and pop); (2) multiplexors (`I_LINK`,
  `I_PLINK`); (3) virtio-net as a DLPI style 1 driver; (4) ARP and IPv4
  with ICMP, linked over the DLPI streams, and interface configuration;
  (5) UDP and the sockfs translation to TPI for `AF_INET`
  `SOCK_DGRAM`; (6) TCP; (7) the userland (`ifconfig`, `route`,
  `ping`, `netstat`, a resolver configuration). QEMU's user-mode
  network is the test peer, so the tests need no host privilege.
- **Deferred:** IPv6, congestion control beyond the RFC 5681 basics,
  SACK, multicast routing, IPsec, and NetBSD drivers through an `ifnet`
  shim (the open question above).

## The STREAMS framework (2026-11-22)

- **Messages** are `mblk_t` and `dblk_t` with the DDI names and
  behaviour: `allocb`, `freeb`, `freemsg`, `dupb`, `dupmsg`, `copyb`,
  `copymsg`, `msgdsize`, `pullupmsg`, `adjmsg`, `linkb`, `unlinkb`,
  `esballoc`. A data block and its buffer are one allocation for small
  sizes. Reference counts are atomic, so a duplicate may be freed on
  any processor.
- **Queues** are `queue_t` pairs with `qinit`, `module_info` and
  `streamtab`, and the DDI queue routines: `putq`, `getq`, `putbq`,
  `insq`, `rmvq`, `flushq`, `flushband`, `qenable`, `noenable`,
  `enableok`, `canput`, `canputnext`, `bcanput`, `bcanputnext`,
  `put`, `putnext`, `qreply`, `putctl`, `putctl1`, `qsize`. Messages are
  ordered high priority first, then by band, then by arrival. Band
  flow control has a `qband` per band in use.
- **Synchronisation** is by synchronisation queues, Belka's own design
  in the manner of Solaris perimeters. A module or driver declares a
  level when it is registered: one per queue pair (the default) or one
  per module (shared by every instance). Its put and service
  procedures, its open and close, and its timeout and buffer callbacks
  run only while their syncq is claimed, one thread at a time. A put
  to a claimed syncq is not a wait: the message is left on the syncq
  and run by the thread that holds it before it lets go, so no lock is
  held across a call into another module and calls in a loop (a
  `qreply` into the caller's own pair) cannot recurse. A procedure
  without a syncq (Solaris's "MT-hot") is not offered until a module
  needs one.
- **Context:** put and service procedures run in thread context.
  Interrupt handlers and callouts do not call `put`; they use `putq`
  and `qenable` (queue state is under an interrupt-safe spin lock), and
  `qtimeout` and `qbufcall` callbacks are moved into thread context.
  STREAMS service threads, one per processor, run the syncqs that have
  work.
- **Topology:** `q_next`, and Belka's private back links for
  back-enabling, change only while the stream's plumbing lock and the
  syncqs of every queue pair whose links change are held (claimed in
  address order, so two plumbing operations cannot deadlock over a
  shared per-module syncq). Walks of more than one hop (`canputnext`
  and back-enabling) take the stream's spin lock.
- **Stream head:** read modes (`RNORM`, `RMSGD`, `RMSGN`, with
  `RPROTNORM`, `RPROTDAT`, `RPROTDIS`), `getmsg`, `putmsg`, `getpmsg`,
  `putpmsg`, `I_PUSH`, `I_POP`, `I_LOOK`, `I_FIND`, `I_FLUSH`,
  `I_FLUSHBAND`, `I_NREAD`, `I_STR`, `I_SRDOPT`, `I_GRDOPT`, `I_PEEK`,
  `I_CANPUT`, `I_ATMARK`, `I_SWROPT`, `I_GWROPT`, `I_SETCLTIME` and
  `I_GETCLTIME`, with `M_ERROR`, `M_HANGUP`, `M_FLUSH`, `M_SETOPTS`,
  `M_SIG` and `M_PCSIG` handled at the head. `I_SETSIG` keeps its
  registrations. Transparent ioctls (`M_COPYIN`, `M_COPYOUT`), file
  descriptor passing, `I_LIST` and multiplexors come next.
- **Numbers:** `getmsg` and `putmsg` are system calls 85 and 86, as in
  System V. `getpmsg` and `putpmsg` are Belka's 246 and 247. The ioctl
  numbers, message types and flags are System V's.
- **The loopback driver** `/dev/strloop` becomes a STREAMS driver: each
  open is a clone, and what is written comes back up, data and
  protocol messages alike. `strpass`, a module that passes everything
  on, and `strup`, which upper-cases data going up, exist to test
  pushing and popping.

## STREAMS multiplexors (2026-11-22)

- **Links:** `I_LINK` and `I_PLINK` take a stream's descriptor and put
  that (lower) stream under the controlling stream's driver, which must
  have `st_muxrinit` and `st_muxwinit`. The lower stream's head is set
  aside, not freed: a queue pair of the driver's lower `qinit`s takes
  its place at the top, in the syncq of the driver's upper pair (a
  driver whose instances share links, as IP will, uses `SQLVL_MODULE`).
  The driver learns of the link from an `M_IOCTL` whose data is a
  `linkblk` (`l_qtop`, its upper write queue or NULL for a persistent
  link; `l_qbot`, its lower write queue; `l_index`, the multiplexor
  id). The id is what the ioctl returns. If the driver refuses, the
  lower stream gets its head back.
- **The lower stream** is held by the link (its file), so its descriptors
  may be closed. While linked it cannot be read, written, plumbed or
  given ioctls (`EINVAL`), and `poll` reports `POLLNVAL`. Only the
  controlling stream itself would make a cycle, since a linked stream
  cannot link.
- **Unlinking:** `I_UNLINK` takes an id, or `MUXID_ALL`, of the
  controlling stream's links; `I_PUNLINK`, on any stream of the same
  driver, undoes persistent ones. The driver is told with an `M_IOCTL`
  of the same command and `linkblk`; a refusal keeps the link. A
  controlling stream's last close unlinks its ordinary links first,
  whatever the driver answers, and persistent ones stay until
  `I_PUNLINK`. The waits for these answers are not interrupted by
  signals, since a link the driver may have taken cannot be abandoned.
- **Testing:** `strmux` (`/dev/strmux`) is a small multiplexing driver
  for `strtest`: links of loopback streams (with modules), selection,
  persistent links, `MUXID_ALL`, and two levels of multiplexing.

## virtio-net and DLPI (2026-11-22)

- **DLPI version 2, style 1, connectionless:** each virtio-net device
  is a STREAMS device, `/dev/vionetN` (N from 0, in attach order); every
  open is a clone stream already attached to that device. The
  primitives are `DL_INFO_REQ`, `DL_BIND_REQ`/`DL_UNBIND_REQ` (the SAP
  is the Ethernet type), `DL_UNITDATA_REQ`/`DL_UNITDATA_IND`,
  `DL_PHYS_ADDR_REQ`, and `DL_PROMISCON_REQ`/`DL_PROMISCOFF_REQ` at the
  `DL_PROMISC_SAP` and `DL_PROMISC_PHYS` levels; others get
  `DL_ERROR_ACK` (`DL_ATTACH_REQ` with `DL_OUTSTATE`, the rest
  `DL_BADPRIM` or `DL_UNSUPPORTED`). The structures, values and
  `<sys/dlpi.h>` are the DLPI ABI's, with 32-bit fields.
- **Addresses** are the six-byte station address followed by the
  two-byte SAP in host order (`dl_sap_length` -2). A `DL_UNITDATA_REQ`
  may give six bytes, for the stream's own SAP. Frames for another
  station reach only `DL_PROMISC_PHYS` streams; broadcast and multicast
  frames reach every bound stream of their type. Frames of 802.3 length
  type are not delivered yet.
- **The device:** `VIRTIO_F_VERSION_1` with `VIRTIO_NET_F_MAC` and
  `VIRTIO_NET_F_STATUS`; no offloads, no mergeable buffers, no control
  queue. Receive buffers are 2 KB, posted at attach. Completions run in
  the queues' interrupt threads: received frames are handed to a
  kernel thread per device (`vionetN`), which copies each into a
  message for every stream that wants it (`put` into the stream's
  driver read queue), and reposts the buffer. A frame that finds the
  stream full is dropped. Transmission copies the frame into one
  buffer and queues it without waiting (`vq_trysubmit`); a full ring
  drops it.
- **STREAMS devices are registered at attach** (`strdev_register`), and
  devfs makes a clone node for each. The device's unit is in the top
  eight bits of a stream's minor number.
- **Tests:** the self-test boot has a virtio-net device on QEMU's
  user-mode network, with a fixed station address. `nettest` binds the
  ARP type, asks QEMU's gateway (10.0.2.2) for its address, and checks
  the reply and the DLPI state machine.
- **Superseded in part (2026-11-24):** the DLPI provider is now shared
  by every Ethernet driver (`net/dlether.c`); see "The Ethernet DLPI
  provider, and Intel's 82574" below. What it does is unchanged.

## ARP and IPv4 (2026-11-22)

- **IP is a multiplexing STREAMS driver, `/dev/ip`,** one perimeter for
  all its streams (`SQLVL_MODULE`), with ARP inside it (`netinet/arp.c`)
  rather than a module of its own. Its lower streams are DLPI streams:
  two per interface, one that IP binds to the IPv4 type and one to ARP,
  both linked with `I_PLINK` and then named with `IP_IFATTACH` (Belka's
  ioctls, `<sys/ipconf.h>`, through `I_STR`, superuser only). IP asks
  each for its station address and binds it, and answers the
  `IP_IFATTACH` when both binds are acknowledged. Unlinking one detaches
  the interface. `lo0`, 127.0.0.1/8, is always there.
- **Configuration:** `IP_IFADDR` gives an interface its address and
  mask (and a route to its subnet), `IP_ROUTEADD` and `IP_ROUTEDEL` add
  and remove routes (the longest prefix wins), and `IP_IFLIST`,
  `IP_ROUTELIST` and `IP_ARPLIST` report the tables. The socket ioctls
  (`SIOCSIFADDR` and the rest) will be built on these.
- **The upper interface is datagrams:** an upper stream binds a protocol
  number (`IP_BINDPROTO`) and then writes whole IPv4 datagrams as
  `M_DATA`; IP fills in the version, length, identification, checksum,
  TTL if zero, and the source if it is 0.0.0.0, routes the datagram,
  and resolves the next hop. An upper stream reads in message-discard
  mode (`M_SETOPTS` at open), one datagram per read. Received datagrams for us are delivered,
  header and all, to every upper stream bound to their protocol: the
  transports (UDP, TCP) will be drivers with an IP stream linked below
  them, and a raw stream is what raw sockets will use. TPI is spoken by
  the transports, not by IP.
- **What IP does itself:** checks headers and checksums, answers ICMP
  echo requests and sends protocol-unreachable for a protocol no stream
  has bound; local addresses are delivered without the network. It does
  not forward, fragment or reassemble yet: a datagram larger than the
  interface's MTU is refused, and fragments are dropped. (Superseded:
  forwarding and fragmenting came after; see "IP forwarding". Fragments
  for us were still dropped; see "IP reassembly" for after.)
- **ARP** (RFC 826) keeps a cache per interface: an unresolved next hop
  holds up to three datagrams, and the request is sent again each second
  up to three times, after which they are dropped. Entries last twenty
  minutes. Requests for our address are answered, and senders already in
  the cache or asking for us are recorded. Timers run with `qtimeout` in
  IP's perimeter.
- **Testing:** `iptest` links `vionet0` under `/dev/ip`, configures
  10.0.2.15/24 with QEMU's gateway as the default route, and pings the
  gateway, its own address and 127.0.0.1 through a raw ICMP stream.

## UDP and sockets over TPI (2026-11-23)

- **UDP is a multiplexing STREAMS driver, `/dev/udp`** (`netinet/udp.c`),
  one perimeter for all its streams, like IP. Its one lower stream is an
  IP stream, linked with `I_PLINK` by the superuser (a second is
  `EBUSY`); UDP then binds it to protocol 17 with an `IP_BINDPROTO` of
  its own, under `kcred`. Nothing plumbs it at boot yet: `udptest` does,
  and takes it apart again, until there is a start-up program for the
  network.
- **The upper side is TPI, connectionless** (`<sys/tihdr.h>`, the
  messages and numbers of the System V interface, fields 32 bits wide):
  `T_INFO_REQ`, `T_BIND_REQ` (no address, or port 0, picks a free port
  from 49152 up; below 1024 needs a superuser opener), `T_UNBIND_REQ`,
  `T_UNITDATA_REQ` out and `T_UNITDATA_IND` in, with `sockaddr_in`
  addresses. `T_CONN_REQ` and `T_DISCON_REQ` name and forget a peer, as
  connected UDP sockets need: the peer is where `M_DATA` (a `write`)
  goes, and the only source heard from. They are answered by `T_OK_ACK`
  alone; there is no `T_CONN_CON`. Options (`T_OPTMGMT_REQ`) are not
  supported yet.
- **The division of work with IP:** UDP sends its datagrams with an IP
  header whose source may be 0.0.0.0, and IP, once it has chosen the
  source, computes the UDP checksum, which covers it (for every UDP
  datagram an upper stream sends, so a raw stream's own is replaced).
  UDP checks the checksums of what arrives, delivers each datagram to
  the most specific endpoint (connected, then bound to the address, then
  to any), a broadcast to all that match, and hands a datagram nobody
  takes back to IP in an `M_CTL` (`struct ip_ctl`, `IPCTL_UNREACH`), for
  IP to answer with ICMP port unreachable unless it was a broadcast.
  Binding a specific address is not checked against the interfaces yet.
- **Sockets are the socket layer's own vnodes over a stream**
  (`kern/sock_tpi.c`): `socket(AF_INET, SOCK_DGRAM)` opens a `/dev/udp`
  stream inside the kernel (`str_kopen`), and `bind`, `connect`,
  `sendto` and `recvfrom` become TPI messages on it. Requests are one at
  a time per socket, and each waits for its acknowledgement, the one
  high priority message at the head (one left behind by a signal is
  discarded before the next request). Data is the head's ordinary
  messages, which the socket layer takes whole (`str_kgetmsg`), without
  the head's reader lock, so a thread waiting for data does not hold up
  another's `bind`. A datagram is read once: what does not fit is
  discarded (`MSG_TRUNC` gives its whole length), and `MSG_PEEK` copies
  without taking it. An unbound socket is bound to a free port by its
  first `sendto` or `connect`; `connect` to `AF_UNSPEC` disconnects; the
  address 0.0.0.0 to send or connect to means this host (127.0.0.1).
  `read`, `write` and `poll` work on the socket as on the stream.
- **New system calls** (Belka's numbers): `sendto` 248, `recvfrom` 249,
  `getsockname` 250 and `getpeername` 251. `send` and `recv` remain, and
  for a local stream socket the address is refused (`EISCONN`) or empty.
  `getsockname` gives a bound local socket its path.
- **Testing:** `udptest` plumbs UDP over IP, speaks TPI to a `/dev/udp`
  stream directly, and exercises the sockets over the loopback,
  including port unreachable as seen on a raw ICMP stream (which also
  shows the checksums IP computes are the ones UDP accepts).

## TCP and stream sockets over TPI (2026-11-23)

- **TCP is a multiplexing STREAMS driver, `/dev/tcp`** (`netinet/tcp.c`),
  one perimeter for all its streams and its timer, plumbed as UDP is:
  one IP stream linked by the superuser, bound to protocol 6. The timer
  is a tick every 100 ms (`qtimeout` on the lower stream), which counts
  down each connection's retransmission and TIME_WAIT timers and times
  round trips. Unlinking the IP stream ends every connection
  (`ENETDOWN`).
- **The upper side is TPI, connection-oriented with orderly release**
  (`T_COTS_ORD`). `T_BIND_REQ` with a `CONIND_number` listens.
  `T_CONN_REQ` is answered by `T_OK_ACK` and then `T_CONN_CON` or
  `T_DISCON_IND`. A listener's connections are indicated by `T_CONN_IND`
  once their handshake is done, and `T_CONN_RES` gives one to another
  stream, named by the `ACCEPTOR_id` from that stream's
  `T_CAPABILITY_ACK`. `T_CAPABILITY_REQ`/`ACK` and `T_ADDR_REQ`/`ACK`
  are Belka's as Solaris numbers them. Data goes down as `M_DATA` or
  `T_DATA_REQ` and comes up as `M_DATA`; `T_ORDREL_REQ` and
  `T_ORDREL_IND` are the FINs; `T_DISCON_REQ` aborts (a reset), and
  `T_DISCON_IND` gives the reason as an errno.
- **The protocol** (RFC 9293): the whole state machine, the MSS option,
  retransmission with RFC 6298's timer (at least 200 ms, at most 60 s,
  doubling, twelve tries, six for a SYN), slow start and congestion
  avoidance, window probes on a closed window, challenge ACKs for an
  in-window SYN (RFC 5961), and TIME_WAIT of 60 s. Windows are at most
  64 KB (no scaling); the send buffer is 64 KB. A connection's own
  address may be left to IP, which fills in the source and computes
  the checksum as for UDP; TCP learns it from the first segment back.
  Not yet: window scaling, timestamps, SACK, fast retransmit, queueing
  out-of-order segments (they are dropped and acknowledged), Nagle's
  algorithm, delayed ACKs, urgent data, keep-alives, options, ICMP
  errors, and RFC 6528 initial sequence numbers. (Superseded, but for
  timestamps, SACK and urgent data: see
  [TCP: sequence numbers, congestion, windows and the path MTU](#tcp-sequence-numbers-congestion-windows-and-the-path-mtu-2026-11-24).)
- **Closing a stream closes as a socket does:** with a FIN, finished by
  the connection as an orphan, or with a reset if data the reader never
  took was waiting (in TCP or at the stream head, which TCP looks at in
  its close, the head's syncq claimed with its own). An orphan in
  FIN_WAIT_2 waits at most 60 s. Binding is as with `SO_REUSEADDR`: a
  port is held only by a listener or a bound socket not yet used, so a
  server may bind again over its old connections. (Superseded: binding
  over connections now takes `SO_REUSEADDR`; see "Socket options, and
  connecting without waiting".)
- **Stream sockets** (`kern/sock_tpi.c`): `listen` binds again with the
  backlog; `accept` takes a `T_CONN_IND`, opens a new `/dev/tcp`
  stream, and answers with `T_CONN_RES` (a connection reset before then
  is passed over); `connect` waits for the connection, even on a
  non-blocking socket, until there is a way to wait for it that `poll`
  can see (superseded: see "Socket options, and connecting without
  waiting"); `send` writes 16 KB messages under the stream's flow control;
  `recv` takes what is there, putting back what does not fit, one reader
  at a time (`ts_rdlock`). The end of the peer's data reads as 0, a
  reset as `ECONNRESET` once and then 0. `shutdown` (Belka's system
  call 252) sends `T_ORDREL_REQ` for `SHUT_WR`; writing after it is
  `EPIPE` and `SIGPIPE`. A local socket's ends have no `shutdown` yet.
  `getsockname` asks TCP (`T_ADDR_REQ`), since IP chose the address.
- **Testing:** `tcptest` plumbs TCP over IP, speaks TPI to `/dev/tcp`
  directly, and over the loopback checks listening and binding,
  connect and accept, a mebibyte each way between two processes,
  closed windows holding a writer off, a SYN dropped by a full backlog
  and sent again, orderly release, a refused connection and a reset.
  The loopback loses nothing else, so retransmission of data is not
  yet exercised.

## The network at boot (2026-11-23)

- **`netcfg`** (`usr/sbin/netcfg`, `/sbin/netcfg`) is Belka's own
  plumbing and configuration program, over STREAMS links and the
  `/dev/ip` ioctls: `plumb` links an IP stream under UDP and TCP
  (persistently; one already linked is left), `attach` makes an
  interface of a DLPI device (linked twice under IP, then
  `IP_IFATTACH`), `addr` and `route` configure it, and `show` lists
  interfaces, routes and the ARP cache. It is not BSD's `ifconfig` or
  `route`, whose socket ioctls Belka does not have yet; those can come
  later, built on the same ioctls.
- **`/etc/rc.d/network`** runs it at boot, before `NETWORKING`: `plumb`
  always (with `network=YES`, the default), then each interface in
  `net_interfaces` whose device exists, with its `ifaddr_<name>`, and
  the default route through `defaultroute`. A missing device is passed
  over, a failure is an error. There is no DHCP client (superseded: see
  [DHCP](#dhcp-2026-11-23)): the shipped
  `/etc/rc.conf` sets up QEMU's user-mode network (`vionet0`,
  10.0.2.15/24, gateway 10.0.2.2), and the normal and test boots now
  have the virtio-net device (`QEMU_ROOT_FLAGS`). The kernel self-test
  boot does not run `rc`; its tests plumb what they use themselves.
- **Testing:** `boottest` checks that the boot configured `vionet0`
  and the default route, pings the gateway, uses TCP and UDP sockets
  over the loopback, and runs `netcfg show`.

## Socket options, and connecting without waiting (2026-11-23)

- **`setsockopt` and `getsockopt` are Belka's system calls 253 and
  254.** Levels and names are numbered as Linux numbers them, which is
  what mlibc's headers already say: `SOL_SOCKET` 1, `SO_REUSEADDR` 2,
  and so on; `TCP_NODELAY` 1, `TCP_MAXSEG` 2, `TCP_KEEPIDLE` 4,
  `TCP_KEEPINTVL` 5 and `TCP_KEEPCNT` 6 (`<netinet/tcp.h>`). An int
  option takes at least an int (`EINVAL` if shorter); `getsockopt` copies
  at most the caller's length and sets it to what it copied. An unknown
  option, or one only to be read, is `ENOPROTOOPT`; a value the option
  will not take is `EINVAL`. On anything but a socket, `ENOTSOCK`.
- **The provider has a socket's options, by `T_OPTMGMT_REQ`.** The
  socket layer sends one option at a time: `T_NEGOTIATE` with the value
  to set it, `T_CURRENT` with none to read it. The options follow the
  request as XTI lays them out, a `struct t_opthdr` (length, level,
  name, status) and the value, each at a multiple of 4 bytes; the
  acknowledgement has each option back with its value and status
  (`T_SUCCESS`, `T_FAILURE`, `T_READONLY`, `T_NOTSUPPORT`), and its
  `MGMT_flags` is the worst of them. The levels and names are the
  sockets' own rather than XTI's. Other flags (`T_CHECK`, `T_DEFAULT`)
  are `TBADFLAG`. `netinet/tpi_opt.c` takes a request apart and builds
  the acknowledgement for UDP and TCP, each with its own function for
  one option. `SO_TYPE`, `SO_ERROR` and `SO_ACCEPTCONN` are the socket
  layer's, not the provider's.
- **TCP's options** are kept per stream until it binds and in its
  connection's state after; a listener's connections take the
  listener's, and the stream that accepts one takes them from it.
  `SO_REUSEADDR` now decides binding over connections: without it, any
  connection on the port (in TIME_WAIT, say) makes the bind
  `EADDRINUSE`; with it, only a listener or another bound socket does.
  `SO_KEEPALIVE` probes a connection with nothing in flight that has been
  idle `TCP_KEEPIDLE` seconds (7200 by default), every `TCP_KEEPINTVL`
  (75), with a segment just behind the window that the peer must answer;
  `TCP_KEEPCNT` (8) unanswered drop it with `ETIMEDOUT`. `SO_RCVBUF` is
  the receive window, 2 KB to 64 KB (a smaller one takes back no window
  already offered); `SO_SNDBUF` the send buffer, 2 KB to 256 KB.
  `SO_LINGER` with a time of 0 makes close a reset; a longer time is kept
  but not waited for, close never blocking. `TCP_NODELAY` is kept and
  reported, but there is no Nagle's algorithm for it to turn off.
  `TCP_MAXSEG` reads the peer's MSS (536 until it says).
- **UDP's options:** `SO_REUSEADDR` lets sockets share a port when both
  have it (a datagram goes to the most specific endpoint, a broadcast to
  each); `SO_BROADCAST` is needed to send to or connect to
  255.255.255.255 (`EACCES` without it); `SO_RCVBUF` is the stream
  head's high-water mark, set by `M_SETOPTS`, so datagrams past it are
  dropped (64 KB by default, which UDP now sets when its stream opens:
  the head's 5 KB was too little); `SO_SNDBUF` is only kept. A
  directed broadcast is not yet recognised as one.
- **A non-blocking connect returns `EINPROGRESS`** (or finishes at once:
  0, or the refusal). The socket is writable when the connection is
  made or has failed; `SO_ERROR` then gives the failure, once, and a
  second `connect` gives `EALREADY` while it goes on, `EISCONN` once
  made, and the failure (once) if it failed. A blocking connect a signal
  interrupts goes on in the same way. A send or receive also gives a
  pending error first.
- **`poll` sees a socket's state, not just its stream head's.** The
  head holds acknowledgements and indications as well as data, so the
  socket corrects what the head says: before it does, it takes any
  `T_CONN_CON`, `T_ORDREL_IND` or `T_DISCON_IND` at the front of the
  head without waiting (and a listener's `T_DISCON_IND` for a connection
  it never had), unless a reader has the stream (`ts_rdlock` is tried,
  not waited for). A connecting socket is neither readable nor writable;
  a listener is readable with a connection waiting and never writable;
  an unconnected stream socket is writable and hung up, as Linux has it;
  the end of the peer's data makes it readable for good; a pending error
  is `POLLERR`; and the head's high-priority messages and bands are not
  the socket's (`POLLPRI` is not reported). A change of state made by a
  reader wakes pollers again.
- **Closing a socket never waits.** A stream's last close waits up to
  15 s for its output to drain; a socket's passes `FNONBLOCK` down, so a
  close behind a peer's shut window returns at once, and the connection
  sends what is left as an orphan (TCP takes what is still on its write
  queue when the stream closes).
- **A local socket** reports `SO_TYPE`, `SO_ERROR` (always 0),
  `SO_ACCEPTCONN` and its fixed 4 KB buffer for `SO_SNDBUF` and
  `SO_RCVBUF`, and takes no options.
- **Not yet:** `SO_RCVTIMEO` and `SO_SNDTIMEO`, `SO_LINGER`'s wait,
  `SO_OOBINLINE`, `SO_DONTROUTE`, the IP level (`IP_TTL`, `IP_TOS`,
  multicast), `SO_RCVLOWAT`, and ICMP errors to sockets. The keep-alive
  timeout is not tested: over the loopback the peer always answers.
  `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT` are extensions, as
  on Linux and the BSDs, which POSIX 2001 programs do not see.
- **Testing:** `tcptest` sends `T_OPTMGMT_REQ` to `/dev/tcp` directly,
  then checks each option through the sockets (defaults, setting, the
  clamped sizes, read-only and unknown options, short and cut-short
  values, `ENOTSOCK`, a local socket's), a listener's options passing to
  its connections, binding with and without `SO_REUSEADDR`, a smaller
  `SO_RCVBUF` holding a writer off sooner, keep-alive probes counted on
  `lo0` with the connection alive after, an abortive close, non-blocking
  connects that succeed and that are refused (`poll`, `SO_ERROR`,
  `connect` again), and what `poll` says in each state. `udptest` checks
  the socket's own options, sharing a port, `SO_BROADCAST`, and
  `SO_RCVBUF` dropping what does not fit.

## IP forwarding (2026-11-23)

- **IP forwards when told to:** `IP_SETFORWARD` (an int, 1 or 0, the
  superuser's) turns it on and off, and `IP_GETFORWARD` returns which
  (both `/dev/ip` ioctls, numbered after `IP_BINDPROTO`). It starts off.
  `netcfg forwarding on`, `off`, or with neither, says; `netcfg show`
  says too. At boot `/etc/rc.d/network` sets it from `ip_forwarding` in
  `rc.conf` (`NO` by default).
- **A datagram not for us is forwarded** (RFC 1812 5.2 and 5.3) by the
  routing table, the longest prefix, with its TTL one less and its
  header checksum made again. It is not forwarded if it came to a link
  broadcast or multicast address (DLPI's `dl_group_address`), if its
  source or destination is a martian (0/8, 127/8, the broadcast,
  multicast and reserved), or if its source is ours. ICMP errors go back
  to the sender: time exceeded for a TTL of 1 or less, host unreachable
  for no route (network unreachable being obsolete), and fragmentation
  needed, with the next hop's MTU (RFC 1191), for one too big that may
  not be fragmented. A datagram that leaves by the interface it came in
  on, from a sender on that interface's subnet, brings the sender a host
  redirect to the better gateway, and is forwarded all the same
  (not when it has IP options, which a source route may be among).
- **ICMP errors** now follow the rules for them (RFC 1122 3.2.2, RFC
  1812 4.3.2.7): none about an ICMP error (an echo request may have
  one), a fragment but the first, or a datagram to or from a broadcast
  or multicast address or from 0.0.0.0. One about a datagram for us
  comes from the address it was sent to; one about a forwarded datagram
  from the address IP chooses toward the sender. They are not rate
  limited yet. (Superseded: see "ICMP errors to the transports".)
- **IP fragments** what is too big for the interface it leaves by, ours
  as well as forwarded (RFC 791): pieces of a multiple of 8 bytes, the
  first with the whole header, the rest with only the options marked to
  be copied; a datagram that may not be fragmented is dropped (a
  forwarded one with the ICMP error above). A fragment is forwarded as
  any datagram is, and fragmented again if need be. Reassembly is still
  to come: fragments for us are dropped. (Superseded: see "IP
  reassembly".) ARP holds at most three
  datagrams for a hop it is resolving, so a datagram of more fragments
  than that to a hop not yet resolved loses the rest.
- **Static routes** are added at boot by `/etc/rc.d/staticroute` (after
  `network`, before `NETWORKING`, when `staticroute` is set, as it is by
  default) from `/etc/routed/static_route.conf`: a route a line,
  destination (`address/length` or `default`) and gateway, `#` to the
  end of a line a comment, each added with `netcfg route add`; a line
  that cannot be added is reported, and the rest are added. The
  directory is where a routing daemon's configuration will go.
  `defaultroute` in `rc.conf` still gives the default route, which DHCP
  will set later.
- **Not yet:** a routing daemon, ICMP redirects taken by a host, the
  router alert and source route options, rate limiting ICMP errors,
  directed broadcasts (a broadcast to one of our subnets is ours, never
  forwarded), multicast forwarding, and a per-interface or counted view
  of what was forwarded.
- **Testing:** the self-test boot has two more virtio-net devices
  (`vionet1` and `vionet2`) on a QEMU hub of their own. `fwdtest`
  attaches `vionet1` under IP as 10.1.0.1/24, with a route to
  10.9.0.0/16 through 10.1.0.3, and plays 10.1.0.2 and 10.1.0.3 through
  raw DLPI streams on `vionet2`, answering IP's ARP requests. It checks
  that nothing is forwarded while forwarding is off; that, on, a
  datagram comes back out to the gateway's station with its TTL one
  less and the sender gets a redirect; time exceeded, host unreachable,
  nothing for a link broadcast or a martian source, and no redirect for
  a sender off the subnet; and that a datagram of ours of 3000 bytes
  goes out as three fragments that make it whole again, and one that
  may not be fragmented goes nowhere. Fragmentation needed is not
  tested, the hub's two links having one MTU. `boottest` checks the
  route a test image adds to `static_route.conf`, and that forwarding is
  off.

## IP reassembly (2026-11-23)

- **Fragments for us are reassembled** (RFC 791 3.2, RFC 815) by
  `netinet/ip_reass.c`, before the datagram goes up as any other does.
  A datagram's fragments wait in a queue of their own, keyed by source,
  destination, protocol and identification, kept in order of offset;
  when the last fragment has set the length and every byte up to it has
  come, the first fragment's header, with the whole length and no
  offset or more-fragments flag, heads the data of all of them, and its
  checksum is made again. Forwarded fragments are not reassembled.
- **What would make a datagram ambiguous drops all of it:** a fragment
  that overlaps one held, unless it is the same one again (which is
  ignored); one past the end the last fragment set, or a last fragment
  that would end before one held; one but the last whose length is not
  a multiple of 8, or of none; and one that would make the datagram
  longer than 65535 bytes. RFC 791 allows overlaps, but no sender makes
  them, and the overlapping attacks on reassembly need them (RFC 5722
  forbids them for IPv6 on those grounds).
- **Bounded:** 32 datagrams at once, 64 fragments to one, and 256 KB
  held in all; a new datagram, or a fragment that would go over, first
  makes way by dropping the queue with the least life left.
- **A queue lives 30 seconds** (`IP_REASSLIFE`), aged by a one-second
  tick: the ARP tick of the interface its first fragment came by
  (`ip_reass_tick`), so that IP needs no timer of its own. A queue that
  dies with its first fragment held sends its sender time exceeded,
  code 1 (RFC 792); one without, nothing. When an interface goes, its
  queues go.
- **Testing:** `fwdtest` sends fragments from the hosts it plays and
  reads the datagrams on a raw stream: three in order make the whole
  datagram; out of order, the first twice, too; one overlapping
  another makes nothing, as does one past 64 KB; and a first fragment
  alone brings time exceeded, code 1, quoting it, after 30 seconds. The
  wait makes `make test` take half a minute longer, so its timeout is
  now 180 seconds.

## ICMP errors to the transports (2026-11-23)

- **ICMP errors IP sends are rate limited** (RFC 1812 4.3.2.8): a bucket
  of 100, filled at 100 a second (`ICMP_ERRBURST`, `ICMP_ERRRATE`),
  shared by every kind, redirects too. Echo replies are not limited.
- **ICMP errors IP receives go up:** destination unreachable, time
  exceeded and parameter problem, with a good checksum, not to a
  broadcast, quoting a datagram of ours (its source one of our
  addresses) with its header and at least the eight bytes after, go to
  the upper streams bound to the quoted datagram's protocol as an
  `M_CTL`, `IPCTL_ICMPERR` (`struct ip_ctl`, which grew `ic_info` for the
  error's second word). Source quench is ignored (RFC 6633), and so,
  for now, are redirects.
- **UDP** gives the error to the endpoint connected to where the quoted
  datagram went, from the port it came from, as a `T_UDERROR_IND`, which
  the socket reports once, on its next call: port and protocol
  unreachable as `ECONNREFUSED`, network unreachable as `ENETUNREACH`,
  the others `EHOSTUNREACH`. An endpoint not connected hears nothing, as
  in the BSDs (RFC 1122 4.1.3.3 leaves it open). Time exceeded,
  parameter problems and need-frag are passed over.
- **TCP** takes an error only if the segment it quotes is still in
  flight on a connection, its sequence number from SND.UNA up to
  SND.MAX, which someone off the path cannot know (RFC 5927 4.1). Port
  or protocol unreachable refuses a connection being made
  (`ECONNREFUSED`, as a reset would); every other error, and any once
  the connection is made, is soft (RFC 1122 4.2.3.9, RFC 5927 4.2): it
  is kept, and given instead of `ETIMEDOUT` if the connection times out
  before anything more is acknowledged. Need-frag is passed over: TCP
  does not set DF, so its segments are fragmented instead; path MTU
  discovery is to come.
- **Testing:** `fwdtest` links UDP and TCP under IP and sends ICMP port
  unreachable quoting what they sent: a connected UDP socket's next
  `recv` fails with `ECONNREFUSED`, once, but not when the quote has
  another port; a TCP connection being made fails with `ECONNREFUSED`,
  but not when the quote has a sequence number never sent. 250
  datagrams with a TTL of 1 draw between 50 and 130 time exceeded.

## Random numbers (2026-11-23)

- **The kernel has a random number generator** (`kern/kern_random.c`):
  the ChaCha20 block function (RFC 8439, written from its description,
  with the original 64-bit counter and nonce), keyed by a 256-bit key
  that is replaced after every request by keystream of its own, so that
  what was given out cannot be found again from the state afterwards.
- **What goes into it:** a pool of 16 words, mixed into the key through
  two ChaCha20 blocks at the next request after anything has been added.
  At boot, before the scheduler starts (`rnd_init`), RDSEED and RDRAND
  when CPUID says the processor has them, and the time stamp counter;
  afterwards, the counter at each device interrupt (`rnd_intr`, which
  skips rather than waits when another processor holds the lock), and
  what is written to `/dev/random` or `/dev/urandom`. Nothing waits for
  more: under QEMU's `max` processor and on any amd64 of the last decade
  RDRAND seeds it at boot. A virtio-rng driver is a possible further
  source.
- **`/dev/random` and `/dev/urandom`** (devfs, the memory devices'
  major, minors 8 and 9, mode 0666) both read it and both add to it
  when written. `getentropy` is POSIX 2024's and is not offered yet
  (superseded: see [getentropy](#getentropy-2026-11-23)).
- **`rnd_lock`** is a spin lock (interrupts add to the pool); keystream
  is made under it 256 bytes at a time, and copied out without it.
- **Testing:** the device self-test checks the block function against
  RFC 8439's test vector (2.3.2) and that draws differ; `boottest` that
  two reads of `/dev/urandom` differ.

## chroot (2026-11-23)

- **`chroot(path)`**, UnixWare's call 61 (`SYS_chroot`), makes a
  directory the process's root: `p_rdir`, which the process structure,
  fork and share groups already carried. Path lookup starts absolute
  paths there, restarts absolute symbolic links there, and stops `..`
  there (and at the system root, for a process whose current directory
  is outside its root). Only the superuser may; the current directory
  stays where it is. Like `chdir`, it is published to a share group
  sharing directories (`PR_SDIR`).
- It is not in POSIX 2001, which dropped it, but it is in the UnixWare
  ABI, and dnscache needs it.

## Name resolution (2026-11-23)

- **The resolver is djbdns' dnscache** (djbdns 1.05, public domain, in
  `contrib/djbdns`), with four of djbdns' tools: `dnsip`, `dnsname`,
  `dnsq` and `dnsqr`. Only those programs and the library sources they
  need are imported; Belka builds them without djbdns' build, choosing
  the variant of each system header itself (`contrib/djbdns/README.belka`
  has the list, and the two changes made: `errno` from `<errno.h>`, and
  `<grp.h>` for `setgroups`).
- **`/etc/rc.d/dnscache`** (after `NETWORKING`, before `DAEMON`, when
  `dnscache` is set, as it is by default) starts dnscache on
  `dnscache_ip` (127.0.0.1), as the user `dnscache` (uid and gid 53),
  chrooted to `/etc/dnscache/root`, with a cache of
  `dnscache_cachesize` bytes, seeded from `/dev/urandom`. Without
  daemontools, the script sets its environment itself, runs it in the
  background deaf to hangups, and keeps its process ID in
  `/var/run/dnscache.pid`. Its log of queries goes to `dnscache_log`
  (`/dev/null`), a fatal error to the console. The clients it serves are
  named under `root/ip/` (`127`: this host), and it starts from the
  servers in `root/servers/@`: the root servers, from IANA's list, or
  with `dnscache_forwardonly` servers that resolve for it, which DHCP
  will set later (superseded: see
  [Name servers from leases](#name-servers-from-leases-2026-11-23)).
- **`/etc/resolv.conf`** names 127.0.0.1, for the C library and djbdns'
  tools; `/etc/hosts` has localhost, and `/etc/services` the common
  services, `domain` among them.
- **mlibc's DNS client is rewritten** (`options/posix/generic/
  lookup.cpp`, for `getaddrinfo`, `gethostbyname` and `getnameinfo`):
  it took a reply's record lengths and compression pointers on trust,
  copying a record's length into a 16-byte address and following
  pointers without bound, used the query ID 123 always, and asked for
  an address's name without reversing it. Now the query's ID comes from
  `/dev/urandom`; a reply is taken only from the server's address and
  port, with the query's ID and question; every name, pointer and
  record is checked against the reply's length, with at most 64
  pointers and 255 bytes a name; an address record must be 4 or 16
  bytes; and the reverse name is the address's bytes, or nibbles, last
  first. It tries twice, 2.5 seconds each. NXDOMAIN is `EAI_NONAME`,
  SERVFAIL or a truncated reply with no answers `EAI_AGAIN`. It still
  asks only the first name server and only over UDP.
- **Testing:** the test image's `servers/@` names 127.0.0.53, where
  `boottest` runs a server of its own, answering for `test.belka`
  (10.20.30.40) and its address's name, and NXDOMAIN for anything else;
  through dnscache, `getaddrinfo`, `getnameinfo`, `dnsip` and `dnsname`
  find them, `nothere.belka` is `EAI_NONAME`, and `localhost.`, which
  dnscache answers itself, is 127.0.0.1. `boottest` also checks that a
  chrooted process sees only its root, and that an ordinary user may
  not chroot.

## getentropy (2026-11-23)

Supersedes the note under [Random numbers](#random-numbers-2026-11-23)
that `getentropy` is not offered.

- **`getentropy(buf, len)`** (POSIX 2024, approved for Belka) is a
  system call, Belka's 255 (`SYS_getentropy`; `NSYSCALL` is now 256),
  as OpenBSD made it rather than a read of `/dev/urandom`: it needs no
  file, so it works in a chroot without `/dev` and with every
  descriptor in use. It fills `buf` from the kernel generator
  (`rnd_get`), at most `GETENTROPY_MAX` (256) bytes, else `EINVAL`; a
  bad buffer is `EFAULT`. It never blocks: the generator is seeded
  before the first process runs.
- **mlibc** already had `getentropy` in `<unistd.h>` and
  `GETENTROPY_MAX` in `<limits.h>`; Belka's sysdeps add `GetEntropy`.
  `abicheck` checks `GETENTROPY_MAX` against `<sys/random.h>`.
- **Testing:** `boottest`, inside a chroot with no `/dev`: two 256-byte
  draws differ, 257 bytes is `EINVAL`, a bad pointer `EFAULT`, zero
  bytes succeeds.

## DHCP (2026-11-23)

Supersedes "There is no DHCP client" under
[The network at boot](#the-network-at-boot-2026-11-23).

- **dhcpcd 10.5.2** (`contrib/dhcpcd`, BSD 2-clause), NetBSD's DHCP
  client, is `/sbin/dhcpcd`: the IPv4 client only (DHCP and ARP
  address probing; no IPv6, IPv4LL, authentication or privilege
  separation), built without its configure script or Makefiles. The
  sources are the upstream release, which NetBSD carries unchanged; its
  generated option table and manual pages are NetBSD's. OpenBSD has
  removed `dhclient` and its successor, `dhcpleased`, needs libevent,
  imsg and routing sockets. What changed is in
  `contrib/dhcpcd/README.belka`, each under `__belka__`.
- **Belka's back ends** (`usr/sbin/dhcpcd`): interfaces, addresses and
  routes by the `/dev/ip` ioctls (`if-belka.c`); and in place of a BPF
  device, a DLPI stream of dhcpcd's own on `/dev/<interface>`, bound to
  IPv4 or ARP, which puts an Ethernet header back on each frame and
  runs dhcpcd's BPF programs over it in user space, checked when set
  (`bpf-belka.c`). The link stream sees every reply before and after
  the address is set, so it stays open in every state; unicast renewals
  go by an ordinary UDP socket on port 68, whose own reads are dropped
  (and which keeps IP from answering the server with port
  unreachable). There is no routing socket: dhcpcd reads the routes
  again whenever it builds them. `arc4random` is `getentropy`.
- **At boot:** `ifaddr_<name>="DHCP"` in `/etc/rc.conf` leaves the
  interface without an address in `rc.d/network`, and `rc.d/dhcpcd`
  (before `NETWORKING`, after `network`; `staticroute` after it) runs
  one manager (`-M`) for all such interfaces with `/etc/dhcpcd.conf`,
  waiting up to `dhcpcd_timeout` (30) seconds for a lease before going
  on in the background. No hook script runs: names stay dnscache's
  (superseded: see
  [Name servers from leases](#name-servers-from-leases-2026-11-23)).
  The shipped `/etc/rc.conf` now leases `vionet0` from QEMU's DHCP
  server, which gives the address and route it had statically.
- **`IP_IFADDR` with address 0** (and mask 0) takes an interface's
  address away: its routes, ARP cache and reassembly queues go with it,
  and it is no longer up. dhcpcd's back end clears an address only if
  it is still the one being dropped, as each interface has one.
- **`SO_PEERCRED`** (Linux's 17, `struct ucred`: pid, effective uid and
  gid) on local stream sockets: a pair gives its maker, a client the
  listener as it was at `listen()`, an accepted socket the client as it
  was at `connect()`; unconnected, `ENOTCONN`. dhcpcd's control socket
  uses it to tell root from others.
- **`MSG_WAITALL`** on local stream sockets: reads go on until the
  request is filled, the peer closes, a signal comes, or the urgent
  mark. TCP sockets do not take it yet (superseded: see
  [Name servers from leases](#name-servers-from-leases-2026-11-23)).
- **`sendmsg` and `recvmsg`** (mlibc's Belka sysdeps) are `sendto` and
  `recvfrom`, the pieces gathered into one buffer or scattered from it
  so that a datagram stays whole. Sending ancillary data is
  `EOPNOTSUPP`; none is received (`msg_controllen` 0, `msg_flags` 0).
- **Testing:** `boottest` checks the lease through the existing
  network checks (the address, the default route, a ping), then that
  dhcpcd is running with its lease and DUID kept, `dhcpcd -U` dumps the
  lease over its control socket, `-k` releases it (address and routes
  gone) and `-n` gets it again; and `SO_PEERCRED` across a socket pair
  and a connection between root and an ordinary user. `libctest` checks
  `sendmsg`/`recvmsg` over several pieces and `MSG_WAITALL`.

## Name servers from leases (2026-11-23)

Supersedes, in [Name resolution](#name-resolution-2026-11-23), that
DHCP would set dnscache's servers later, and in
[DHCP](#dhcp-2026-11-23), that no hook script runs and that TCP sockets
do not take `MSG_WAITALL`.

- **dhcpcd's hook** is Belka's own, `/libexec/dhcpcd-run-hooks`
  (superseded: `/usr/libexec`; see
  [The file system hierarchy](#the-file-system-hierarchy-2026-11-23)) (a
  shell script, from `usr/sbin/dhcpcd`). It keeps the name servers a
  lease offers in `/var/db/dhcpcd/<interface>.dns`, one to a line, and
  removes the file when the lease goes (expiry, release, stop). It does
  nothing else: dhcpcd sets the address, routes and host name itself,
  and `/etc/resolv.conf` keeps naming dnscache.
- **`dnscache_forwarders`** (in `rc.conf`, replacing
  `dnscache_forwardonly`) says whom dnscache forwards to: empty (the
  default), nobody, and it resolves from the root servers; a list of
  addresses; or `DHCP`, the servers the leases offer (the roots while
  there are none). `rc.d/dnscache` writes `root/servers/@` at each
  start, from those or from the root servers' list, now
  `/etc/dnscache/roots`, and sets `FORWARDONLY` when it forwards. With
  `DHCP`, the hook starts dnscache again when a lease's servers change.
- **No `#!`:** Belka's kernel does not run scripts; mlibc's
  PATH-searching `exec` calls give one to `/bin/sh`. dhcpcd runs its
  hook by `execve`, so Belka's change to it does the same on `ENOEXEC`.
- **`MSG_WAITALL` on TCP sockets:** a read goes on waiting after the
  first data until the request is filled, the peer's data ends, it
  disconnects, or a signal comes. UDP sockets take the flag and ignore
  it.
- **`netcfg addr <interface> none`** takes an interface's address away
  (`IP_IFADDR` with address 0), and every route by it.
- **Testing:** `boottest` checks that the hook kept QEMU's name server
  (10.0.2.3) at boot, dropped it on a release and kept it again on the
  rebind; then, with `dnscache_forwarders="DHCP"`, that the hook told of
  a lease naming 127.0.0.54 starts dnscache forwarding there and names
  resolve through a fake server on that address alone, the roots being
  gone; and that `netcfg addr vionet0 none` takes the address and routes
  away. `tcptest` checks `MSG_WAITALL` across two writes 100 ms apart
  and at the end of the peer's data.

## The file system hierarchy (2026-11-23)

The contract is [hierarchy.md](hierarchy.md); in short:

- **Three kinds of software, kept apart.** The base system is `/usr`
  (programs, `/usr/lib64`, `/usr/libexec`, `/usr/share`), a few static
  programs in `/sbin`, its configuration in `/etc` (as shipped, in
  `/etc/defaults`) and its state in `/var/adm` and `/var/db`; it is
  installed and updated whole, never by the package manager. Software
  for the whole system is under `/opt`, `/etc/opt` and `/var/opt` (its
  logs in `/var/opt/log`, with `/var/log` a link there). A user's own
  is under `/opt/people/USER`, `/etc/opt/people/USER` and
  `/var/opt/people/USER`, apart from the shared prefix's names.
- **Back to the defaults** is removing what is under the three `opt`
  trees and putting `/etc` back from `/etc/defaults`, accounts kept;
  later tools will check the base against a manifest.
- **`/bin` is a link to `usr/bin`.** `/sbin` is real and holds only
  static programs, to start and recover the system when the shared C
  library or `/usr` is damaged: init, sh, mount and umount, halt and
  reboot, netcfg, rcorder, ed.
- **Homes** are `/usr/people/USER` (IRIX's), the superuser's `/adm`.
  `/root` and `/home` go.
- **Libraries stay in `/usr/lib64`.** `/usr/lib` is kept for a 32-bit
  UnixWare personality, whose programs name `/usr/lib/libc.so.1`. The
  base runtime linker looks only in `/usr/lib64`; programs under `/opt`
  use their RUNPATH, so nothing there stands in for a base library.
- **`/tmp` and `/var/run` are tmpfs**, empty at every boot; `/var` and
  `/var/tmp` are kept. `/stand` is reserved for the kernel and boot
  files of a disk installation. One file system holds `/`, `/usr` and
  `/etc`.
- **Not yet done:** the tree has not moved; hierarchy.md lists what
  moves (superseded: see
  [The tree moved](#the-tree-moved-2026-11-24)). The reset and verify tools, the manifest, the account history
  and the package manager are to come.

## The tree moved (2026-11-24)

The root image follows [hierarchy.md](hierarchy.md), whose last section
says what moved and what is still to do:

- `/bin`'s programs are in `/usr/bin`, and `/bin` is a link to it;
  `/sbin` holds init, mount, halt and reboot, netcfg, rcorder, and
  static copies of sh and ed. dhcpcd and its hook moved to `/usr/sbin`
  and `/usr/libexec` (its `SBINDIR`, `LIBEXECDIR` and `LIBDIR` with
  them); its manual pages are given the new path as they are installed.
- Root's home is `/adm`; `useradd`'s default is `/usr/people/NAME`.
- `/var/run` is a tmpfs, as `/tmp` is; `/var/log` links to `/var/opt/log`.
- `/usr/sbin/nologin` is new: the system accounts' shell was
  `/sbin/nologin`, which did not exist.
- `tools/mkroot.sh` sets umask 022, so that the image's directories are
  root's alone whatever the host's umask; they were 0775 under an umask
  of 002.
- **Testing:** `boottest` checks the links, `/sbin`'s own static sh and
  ed, the opt and people directories and their modes, root's home, the
  `/var/run` tmpfs, and nologin; `test-console` checks root's PATH and
  that its login starts in `/adm`; `utiltest` that `useradd -m` makes
  the home in `/usr/people`.

## Raw sockets, and the network tools (2026-11-24)

- **Raw sockets:** `socket(AF_INET, SOCK_RAW, proto)`, the superuser's
  (`EACCES` otherwise), for any protocol 1 to 255. One is an upper
  stream of IP's (`/dev/ip`) bound to its protocol (`IP_BINDPROTO`,
  issued from the kernel by the new `str_kioctl`), in `sock_tpi.c`
  beside the TPI sockets. A send is a whole datagram: a header made
  from the socket's `IP_TTL` (0: IP's default), `IP_TOS` and bound
  address, or with `IP_HDRINCL` (always, for `IPPROTO_RAW`) the
  caller's own, whose protocol must be the socket's unless it is
  `IPPROTO_RAW`. IP fills in the length, identification and checksums,
  UDP's and TCP's included, and the source if it is 0.0.0.0. A receive
  is every datagram of the protocol for this host, header and all, its
  source from the header; `connect` gives a default destination and
  hears only that source; `bind` gives the source of the headers made.
  `IPPROTO_RAW` sockets, and any after `shutdown(SHUT_RD)`, hear
  nothing: IP's new `IP_DEAF` ioctl stops a stream's deliveries.
  `SO_BROADCAST`, `SO_RCVBUF` and `SO_SNDBUF` are kept, the others not
  offered (`ENOPROTOOPT`). `IP_TOS`, `IP_TTL`, `IP_HDRINCL` and
  `SOCK_RAW` take Linux's numbers, as mlibc has them; abicheck checks
  them, `IP_DEAF`, `IPPROTO_ICMP` and `IPPROTO_RAW`.
- **ping** is NetBSD's (`contrib/netbsd/sbin/ping`), in `/usr/bin`,
  set-user-ID root and linked statically. Its BSD ICMP names, `n_long`,
  `IP_MAXPACKET`, `MAXTTL`, `timespecadd` and `SA_NOKERNINFO` come from
  the compat headers (`usr/lib/compat/include/netinet`, `compat.h`),
  over mlibc's with `#include_next`.
- **traceroute** is Belka's own (`usr/bin/traceroute`, MIT), as
  NetBSD's, and its `ifaddrlist.c`, have Lawrence Berkeley Laboratory's
  advertising clause. UDP probes to rising ports, or ICMP echo requests
  (`-I`), go out on an `IPPROTO_RAW` socket with their own TTL; the
  answers come on a raw ICMP socket, and only those quoting one of our
  probes count. Set-user-ID root, it gives that up once both sockets are
  open.
- **whois** is NetBSD's (`contrib/netbsd/usr.bin/whois`), with its own
  default servers.
- **DNS tools:** djbdns' dnsmx, dnstxt, dnsipq, dnstrace (with
  dnstracesort) and dnsfilter join dnsip, dnsname, dnsq and dnsqr.
  `host`, `dig` and `drill` (one script) are Belka's shell scripts over
  `dnsqr` (and `dnsq` for `+norecurse`), with the common forms of BIND's
  host and dig and their output's layout; not BIND's, nor ldns'.
- **xdig** (`usr/bin/xdig`) is the C form of the user's 2011 script,
  over djbdns' library: the domain's level, root domain and zone; a
  quick WHOIS from whois.iana.org along its referrals; the zone's
  records asked for type by type (servers no longer answer `ANY`); what
  Google's, Cloudflare's and Quad9's resolvers have, with the zone's
  serial at each; and where web and mail go, with PTRs and the owners
  of the addresses. `-W` and `-P` leave out WHOIS and the public
  resolvers, which are on the Internet.
- **Scripts:** Belka's kernel runs no `#!` scripts, so `host` and `dig`
  are run by a shell, as the shells and `execvp` do for a file the
  kernel refuses.
- **Testing:** `udptest` checks raw sockets (an echo to the loopback
  and its reply, header and all; the options; a UDP datagram with our
  own header taken by a UDP socket; `IPPROTO_RAW` and a shut read side
  hearing nothing; an ordinary user refused). `boottest` runs ping and
  traceroute (UDP and ICMP) to the loopback and ping to QEMU's gateway,
  as root and as an ordinary user, whois against a server of its own,
  and host, dig, drill, the djbdns tools and `xdig -W -P` against its
  fake root, which now answers test.belka's A, MX and SOA, and no data
  (not NXDOMAIN) for its other types.

## TCP: sequence numbers, congestion, windows and the path MTU (2026-11-24)

Supersedes most of the "Not yet" of
[TCP and stream sockets over TPI](#tcp-and-stream-sockets-over-tpi-2026-11-23).

- **Initial sequence numbers** (RFC 6528): a clock of 4 microseconds
  plus SipHash-2-4 of the addresses and ports, keyed from the random
  generator at the first connection (`kern/subr_siphash.c`, written from
  Aumasson and Bernstein's paper; the self-test checks its vectors).
- **Congestion control** (RFC 5681): the first window is min(4 MSS,
  max(2 MSS, 4380 bytes)); slow start and congestion avoidance grow it
  only for data acknowledged, not for our SYN or FIN. Fast retransmit:
  the third duplicate ACK resends the first segment unacknowledged at
  once. Fast recovery is NewReno's (RFC 6582): the window inflated by a
  segment for each further duplicate, a partial ACK resending the next
  hole, a full ACK ending it at min(ssthresh, flight + MSS); a timeout
  ends it too.
- **Out of order:** segments past `rcv_nxt` are held (64 at most, and
  no more bytes than the window), and joined on when the gap fills; each
  draws a duplicate ACK at once, as does the one that fills a gap.
- **Delayed ACKs:** data in order is acknowledged with our next
  segment, at the second segment that comes, or after 200 ms (two of
  TCP's ticks). **Nagle's algorithm:** less than a segment, and all
  there is, waits while anything is unacknowledged, unless
  `TCP_NODELAY` (which now means something), or it carries the FIN.
- **Window scaling** (RFC 7323 2): every SYN of ours offers a shift of
  3, a listener's SYN-ACK only if the peer offered one; both scale, or
  neither. SYN windows are never scaled. The receive buffer is 128 KB
  by default, 256 KB at most (`SO_RCVBUF`).
- **Path MTU discovery** (RFC 1191): TCP sends with DF. "Fragmentation
  needed" quoting a segment in flight (checked as other ICMP errors)
  lowers the MSS to the next hop's MTU less 40 (the next plateau below
  the quoted length if the router gives none; 296 at least) and sends
  again what is in flight; it is not taken as congestion. IP now tells
  the transport the same, with the link's MTU, when a datagram of ours
  with DF is too big for the interface, rather than dropping it unsaid.
  There is no black-hole detection.
- **Not yet:** timestamps (RFC 7323 3), SACK, and urgent data.
- **Testing:** `fwdtest` plays a TCP peer on the wire to a listener of
  ours: the window scale offered and taken (and its effect both ways),
  or not; three connections' initial sequence numbers not a run of a
  clock; a segment out of order held, then both delivered in order;
  two segments drawing one delayed ACK; small writes held by Nagle,
  and not with `TCP_NODELAY`; three duplicate ACKs bringing the lost
  segment and new data past the first window; and "fragmentation
  needed" at 576 bringing 536-byte segments with DF. `tcptest` checks
  the larger `SO_RCVBUF`.

## SSH and inetd (2026-11-24)

The SSH server and client are Dropbear 2026.94 (`contrib/dropbear`, MIT,
with LibTomCrypt and LibTomMath in the public domain), not OpenSSH. It
is small and self-contained, carries its own cryptography, and needs no
OpenSSL. The programs are installed under OpenSSH's names, so that
scripts and habits find them: `dropbear` is `/usr/sbin/sshd`, `dbclient`
`/usr/bin/ssh`, `dropbearkey` `/usr/bin/ssh-keygen`, and `scp` keeps its
name. `dropbearconvert` keeps its own, as OpenSSH has no such program.
Their options stay Dropbear's. The manual pages go in under both names.

- **Built without configure:** `usr/sbin/dropbear/config.h` is written
  by hand from what mlibc and the compat library offer, and
  `localoptions.h` holds Belka's choices. No source file is changed.
- **Privileges:** Dropbear drops to the user at authentication only
  with `setresgid`, which Belka lacks: the kernel keeps saved IDs, but
  there is no call to set the three at once, and mlibc's falls to
  ENOSYS. The setting is off (`DROPBEAR_SVR_DROP_PRIVS 0`), so the
  server stays root until the session's fork. Unix-domain socket
  forwarding needs the drop, so it is off too. `setresuid` and
  `setresgid` (POSIX 2024) would bring the drop back. Superseded the same
  day: see "setresuid and setresgid" below.
- **Off:** X11 forwarding, the SFTP server, re-execution (no
  `fexecve`), zlib, PAM, lastlog, and the old utmp and wtmp. Sessions
  are recorded in utmpx and wtmpx. The seed comes from `/dev/urandom`.
- **Keys:** the host keys (ed25519 and ecdsa by default; rsa when
  asked, as it is slow to make) are in `/etc/dropbear`, made at the
  first boot by `/etc/rc.d/sshd`.
- **Logins:** `sshd -g` refuses root by password, as OpenSSH's
  `prohibit-password` does. Root may still log in by key.

The inetd is ucspi-tcp 0.88's `tcpserver` (`contrib/ucspi-tcp`, public
domain), not a monolithic inetd. One small process listens per
service, with a connection limit and tcprules' access rules (a cdb
compiled from `/etc/inetd/<name>.rules`). It forks the server with the
connection on its standard input and output, and the remote address in
the environment. `/etc/rc.d/inetd` starts one for each line of
`/etc/inetd.conf` (`name port user program args`) on `inetd_address`.
It passes no ident or name lookups (`-RHl0`) and turns off Nagle (`-D`).
`sshd` runs under it by default (`sshd -i -g`). `sshd_standalone=YES`
runs it as a daemon instead. Only TCP is served: no UDP, RPC or
internal services.

`boottest` checks the host keys and inetd's listener. It then logs in
with ssh by a key ssh-keygen made, runs a command as the user, copies
a file both ways with scp, logs in by password, and is refused with a
wrong key and password. Last, it runs a session on a pseudo-terminal
and checks its record in wtmpx with the client's address.
`make run SSH_PORT=n` forwards a port on the host's loopback to the
guest's port 22.

## setresuid and setresgid (2026-11-24)

Belka now has `setresuid`, `setresgid`, `getresuid` and `getresgid`.
They appeared in HP-UX by 1997, were later taken up by the BSDs and
Linux, and are in POSIX 2024. They are Belka's own calls 256 to 259
(`NSYSCALL` is now 260), and mlibc's generic functions reach them
through the `SetResuid`, `SetResgid`, `GetResuid` and `GetResgid`
sysdeps.

- **setres\*id(r, e, s):** an id of -1 leaves that one as it is. With
  an effective user id of 0, any id may be set. Otherwise, each id
  given must already be one of the three the process has, and every
  id is checked before any changes, so a call either does all of it or
  none (EPERM). An id past the largest is EINVAL. Like the other set
  calls, both publish the change to the share group (`PR_SID`).
- **getres\*id(&r, &e, &s):** copy the three ids out, or EFAULT.

This supersedes the privileges bullet of "SSH and inetd" above.
Dropbear now builds with `DROPBEAR_SVR_DROP_PRIVS`, so the server
gives up root once a user is authenticated. Unix-domain socket
forwarding is back. The server keeps group `utmp` (gid 45, new in
`/etc/group`) as its saved group, and raises it again to write the
logout's record. `/var/adm/utmpx` and `wtmpx` are made group `utmp`,
mode 0664, on the image.

`credtest` checks the calls as the superuser and as the user. It sets
each id alone and all three at once, keeps a saved group for a later
`setegid`, and takes back a saved user id 0. It also checks EPERM,
EINVAL and EFAULT. `boottest` checks that the shell's parent, sshd,
runs as the user in an SSH session, and that wtmpx gets the logout's
record as well as the login's.

## The Ethernet DLPI provider, and Intel's 82574 (2026-11-24)

The DLPI provider that was vionet's (see "virtio-net and DLPI") is now
shared by every Ethernet driver, in `net/dlether.c` with
`<sys/dlether.h>`. A driver fills in a `struct dlether` with its name,
station address and transmit routine, registers it with
`dlether_register`, and gives each received frame to `dlether_input`.
The primitives, the state machine and the per-stream filtering are as
they were. A stream's open finds its device with `strdev_priv`, by
major number and unit, and each driver keeps its own major
(`DEV_VIONET_MAJOR` 9, `DEV_E1000G_MAJOR` 13). What is new:

- **`de_promisc`:** an optional hook, called as the first stream turns
  `DL_PROMISC_PHYS` on or the last one turns it off (or closes). Hardware
  that filters unicast frames itself needs it. QEMU's virtio-net takes
  every frame, which had hidden the need.
- **Frames are built by the provider** (header, data, padding to 60
  bytes) and handed to the driver whole. vionet copies them after its
  virtio header.

**e1000g** (`dev/pci/e1000g.c`, `dev/pci/e1000reg.h`) drives Intel's
82574, which QEMU emulates as `e1000e`. It was written from Intel's
public datasheet, and named as illumos names the family. The first
target was to be the older 82540EM (QEMU's `e1000`), but that part has
only INTx interrupts. Belka routes PCI interrupts by MSI-X alone, and
can't map an INTx line to its GSI without the ACPI `_PRT`, for which it
has no AML interpreter. The 82574 has the same register model and
descriptors, with MSI-X.

- **Rings:** one receive and one transmit ring of 128 legacy
  descriptors, each with its own 2048-byte buffer, two to a page, so
  each buffer is physically one piece. Frames are copied in and out.
  The device strips the CRC and pads short frames.
- **Interrupts:** every cause goes to MSI-X vector 0 (IVAR). The
  handler takes the causes from ICR and wakes the device's thread. The
  thread drains the receive ring, reclaims transmit slots, and reports
  link changes. The transmit routine reclaims too, when the ring runs
  short.
- **Filtering:** broadcast and every multicast frame are taken (BAM,
  MPE), as DL_ENABMULTI_REQ is not supported. Unicast is the station's
  own unless a stream is promiscuous (UPE).
- **Station address:** RAL0/RAH0 as the device loads them from its NVM
  at reset.
- **Not yet:** checksum offload, jumbo frames, MSI or INTx (for the
  8254x and the I217–I219), setting up the PHY (it relies on the
  PHY's own autonegotiation), and statistics.

**Testing:** the self-test boot puts an `e1000e` on the hub with
vionet1 and vionet2. `e1000test` checks the station address in
DL_INFO_ACK and DL_PHYS_ADDR_ACK. It sends frames of 1 to 1500 bytes
each way to vionet2 (short ones padded with zeros), and a broadcast.
It checks that a frame for another station is filtered out unless the
stream is promiscuous. It sends 300 frames each way, more than a ring
holds, in order. Then it puts e1000g0 under IP as 10.4.0.1/24 and plays
10.4.0.2 on vionet2: a UDP datagram goes out after IP's ARP request is
answered, and one comes in to a socket.

## NVMe and AHCI disks, and MSI (2026-11-25)

Belka could only find its disks through virtio-blk, so it could not
reach a root file system on a real machine, or under a hypervisor that
does not offer virtio. It now drives NVMe and AHCI (SATA)
controllers, the disks of nearly every machine of the last decade.
Both were written from their public specifications (NVM Express 1.4;
AHCI 1.3.1 and the ATA command set). Both sit under the existing
block-device interface (`<sys/blkdev.h>`): synchronous reads, writes
and flushes, from a thread.

- **nvme** (`dev/pci/nvme.c`, `nvmereg.h`): each namespace with blocks
  is `nvdN`. Attach resets the controller and enables it with an admin
  queue pair. Admin commands are polled, as attach cannot sleep:
  identify the controller, set the number of queues, create one I/O
  queue pair, identify the namespaces (up to four). The I/O queues are
  64 deep, so up to 63 commands can be outstanding from any number of
  threads. Each command has an identifier and a semaphore. The
  completion queue's MSI-X handler records the status and wakes the
  requester. Data is described by PRPs, using a list for more than two
  pages, and each identifier has room for one. Pages are 4 KB (CC.MPS
  0); a controller whose smallest page is larger is refused. A
  transfer is 64 KB at most, or less if MDTS is smaller. A flush is
  sent only if the controller has a volatile write cache. The MSI-X
  vector must be enabled before the completion queue that names it is
  created: a controller refuses an unenabled one.
- **ahci** (`dev/pci/ahci.c`, `ahcireg.h`): each port with an ATA disk
  is `sdN`; ATAPI devices are passed over (q35's CD-ROM, which the ISO
  boots from), as are empty ports. Attach resets the HBA. Each port
  gets one page: its command list, its received-FIS area, and one
  command table with 32 PRDs. FIS receipt is turned on before the
  signature is read, because the signature arrives in the device's
  first register FIS. If no signature comes, the link is reset
  (COMRESET) once. Each port runs one command at a time, in slot 0,
  under a mutex, sleeping until the interrupt: READ and WRITE DMA EXT
  (48-bit LBAs), and FLUSH CACHE EXT. IDENTIFY DEVICE gives the size
  and logical sector size. A task file error stops and restarts the
  port and returns EIO. Only as many ports are used as the register
  BAR maps: ICH9's is 4 KB.
- **MSI:** `pci_msi_establish` gives a device without MSI-X its one
  MSI vector, through the same `intr_establish_msi` that MSI-X uses.
  AHCI uses MSI-X if the HBA has it, else MSI. Nothing yet takes
  INTx, which needs the ACPI `_PRT` (see "The Ethernet DLPI provider,
  and Intel's 82574").
- **Not yet:** NCQ and more than one command per AHCI port, more than
  one NVMe I/O queue, timeouts on a command that never completes,
  TRIM/deallocate, hot plug, and port multipliers.

**Testing:** the self-test boot gives the test disk image to virtio,
NVMe and AHCI, each its own copy (`build/test-disk{,-nvme,-sata}.img`).
The block-device self-tests (single sectors, 64 KB requests, a
request split in two, a buffer whose pages are not physically
adjacent, out-of-range requests, write, read back and flush, and 800
concurrent reads from eight threads) now run on `vd0`, `nvd0` and
`sd0`. The kmem self-test counted every large allocation and expected
none. The NVMe softc is now one that drivers keep, so the test
compares with the count at its start instead. By hand, the boot-test
image boots with its root on NVMe (`root=nvd0`) and on SATA
(`root=sd0`), with vfstab changed to match. boottest then passes
everything except utiltest's checks that name `/dev/vd0` as root.
