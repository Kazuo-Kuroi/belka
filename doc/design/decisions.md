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
- **Not yet:** `VOP_GETPAGE` and mapped files (with the fault path),
  clustering and read-ahead, vnode shaking, and a pager to clean pages
  under memory pressure.

## User processes (2026-11-08)

- **System calls** enter through `syscall` and leave through `sysretq`.
  `iretq` is used instead whenever the frame no longer describes an
  ordinary user context (exec, or a non-canonical %rip).
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
  Each process has exactly one LWP for now, on a kthread of its own
  that joins fair-share time-sharing on entry.
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
- **Not yet:** multiple LWPs, dynamic linking, init and
  sessions, device special files (the console is a private vnode),
  pipes, credentials beyond the kernel's, and resource limits.

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
- **Not yet:** multi-LWP rules (per-LWP pending signals, sigwait),
  `/proc` tracing, core files, terminals and orphaned process groups,
  SIGXFSZ on EFBIG, and permission checks (every process is the
  superuser).

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
