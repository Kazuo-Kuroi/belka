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

## File systems

1. **tmpfs**, for testing.
2. **ext2**, the first practical file system, ported from BSD sources
   (NetBSD's `ext2fs` is preferred because it is BSD-licensed and matches
   the long-term driver model). Licences of the imported files are checked
   file by file.
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
