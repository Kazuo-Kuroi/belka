# Lock Order

Every lock in the kernel, what it protects, and which locks may be held
while taking it. A lock may be taken while holding only the locks listed
before it in its "taken after" column. Update this page in the same
commit that adds or changes a lock.

## Sleeping locks

Mutexes, mrlocks, semaphores, and sync variables may be held across
anything except a spin lock. Their internal spin locks rank as the sync
object locks below.

| Lock | Protects | Notes |
|---|---|---|
| `vfs_mutex` (`kern/vfs_mount.c`) | the mount list, `v_vfsmountedhere`, mount and unmount | Taken briefly by lookup to cross a mount point. Held across `VFS_MOUNT`/`VFS_UNMOUNT`, so a file system's own locks rank below it. |
| `tm_lock`, one per tmpfs mount (`fs/tmpfs`) | the name space, node attributes and link counts, the node list | Taken by `VOP_INACTIVE`; never held while taking `vfs_mutex`. |
| `tn_rwlock` (mrlock), one per tmpfs node | a file's pages and size | After `tm_lock`. Reads and writes take only this. |
| `i_rwlock` (mrlock), one per ext2 inode (`fs/ext2fs`) | a file's data blocks and size | Taken before `em_lock`. Reads take it shared, writes and truncation exclusive. |
| `em_lock`, one per ext2 mount (`fs/ext2fs`) | the name space, block and inode bitmaps, the superblock and group descriptors, the in-core inode table | After `i_rwlock`. Taken by `VOP_INACTIVE`; held across buffer cache calls and disk I/O. |
| `blkdev_lock` (`kern/subr_blkdev.c`) | the block device registry | Held only while registering or looking up. |
| `intr_mutex` (`arch/amd64/amd64/intr.c`) | the interrupt source table and handler lists, for changes | Held across `smp_call_all()` and `delay()` in `intr_disestablish()`; never taken from an interrupt. |

## Spin locks

Listed roughly from outermost to innermost.

| Lock | Protects | Taken after | Notes |
|---|---|---|---|
| sync object locks: `m_lock`, `sv_lock`, `s_lock`, `mr_lock` (`kern/kern_mutex.c`, `kern_synch.c`, `kern_mrlock.c`) | the object's state and sleep queue | a caller's spin lock (`sv_wait`) | A sleeper drops the object's lock before taking its own `kt_lock`. A waker dequeues under it, then drops it before `wakeup_thread()`, so it never touches the object once the sleeper can run; only the mutex handoff takes the new owner's `kt_lock` under `m_lock`. |
| `bc_lock` (`kern/vfs_bio.c`) | the buffer hash, free list, flags of every buffer, and counters | nothing | Never held across I/O; a buffer is marked busy and the lock dropped first. Waiters sleep on `bc_wait` with `sv_wait()`. |
| `kt_lock`, one per thread (`kern/kern_kthread.c`) | the thread's state, priority, run queue membership, wait channel | sync object locks | Held across a context switch by the thread leaving, and released by the next thread in `sched_finish()`. Never hold two, except the switch handover. |
| `dnlc_lock` (`kern/vfs_dnlc.c`) | the name cache | nothing | Never held across `VN_RELE`, which can enter a file system; evicted references are dropped after unlocking. |
| `v_lock`, one per vnode (`kern/vfs_vnode.c`) | `v_count` and `v_flag` | nothing | |
| `wt_lock` (`kern/sched_ts.c`) | the time-share active job list and every job's thread list | `kt_lock` | Redistribution holds it and only try-locks threads, leaving a busy thread for the next tick. |
| `rq_lock`, one per processor (`sys/sched.h`) | that processor's run queue | `kt_lock` | Code holding `rq_lock` takes a `kt_lock` only with `spin_trylock()`. Never hold two run queue locks. |
| `vq_lock`, one per virtqueue (`dev/virtio/virtio.c`) | the rings, free list, and cookies | nothing | The completion callback runs under it and may only wake threads (`sema`, `sv` locks nest inside). Submitters wait for space with `sv_wait()` on it. |
| `pci_conf_lock` (`arch/amd64/amd64/pci_machdep.c`) | the legacy 0xcf8/0xcfc pair; ECAM bus mappings | nothing | |
| `io_lock`, one per I/O APIC (`arch/amd64/amd64/ioapic.c`) | the select/window register pair | nothing | Taken from the hardware interrupt to mask a line. |
| `callout_lock` (`kern/kern_timeout.c`) | the callout list | nothing | Released while a callout function runs. |
| `zombie_lock`, `allthreads_lock` (`kern/kern_kthread.c`) | exited threads; the list of all threads | nothing | |
| `kva_lock` (`vm/vm_kva.c`) | the free KVA range list | nothing | Never held across an allocation; range records are allocated before and freed after. |
| `kz_lock`, one per zone (`kern/kern_kmem.c`) | a zone's slab lists, slabs, and counters | nothing | Never held while calling the page allocator. |
| `kmem_zones_lock` (`kern/kern_kmem.c`) | the list of all zones | nothing | Taken only when a zone is created. |
| `hat_klock` (`arch/amd64/amd64/hat.c`) | the kernel page tables below the PML4, for KVA changes | nothing | Allocates page-table pages, so it is taken before the node locks. Never held across a TLB shootdown. |
| `page_nodes[n].pn_lock` (`vm/vm_page.c`) | node *n*'s free lists, counters, and the free-list fields of its pages | `hat_klock` | Never hold two node locks at once; allocation from a remote node drops the local lock first. |
| `ci_act_lock` (`arch/amd64/amd64/intr.c`) | one processor's action queue | nothing | Held only to queue or dequeue; actions run with it released. Never held while waiting for another processor. |
| `prf_lock` (`kern/subr_prf.c`) | interleaving of `printf` output | any lock | Innermost: `printf` may be called with any lock held. Ignored after a panic. |

## Rules

- Spin locks raise to splhi. Never sleep or block while holding one.
- `prf_lock` is the only lock that may be taken while holding another
  lock of the same rank or deeper. Anything that prints must therefore
  never take another lock from inside the console path.
- After a panic or halt, locks held by a stopped processor are never
  released. Code on those paths (`panic`, `printf`, `mdboot`) must not
  wait for any lock; `printf` skips its lock once `panicstr` is set or
  `smp_halting()` is true.
- Do not call `smp_call_all()` or `hat_kremove()` while holding a spin
  lock that an action on another processor might take: the waiter
  serves its own queue, but cannot release the lock it holds.
