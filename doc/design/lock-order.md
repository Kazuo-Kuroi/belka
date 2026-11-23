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
| `em_lock`, one per ext2 mount (`fs/ext2fs`) | the name space, block and inode bitmaps, the superblock and group descriptors, the in-core inode table | After `i_rwlock`. Taken by `VOP_INACTIVE` and `VOP_RECLAIM`; held across buffer cache calls, disk I/O, and `vn_alloc()`, whose reclaims only try another mount's `em_lock` (`VRC_NOWAIT`) and reclaim under this one when it is the caller's. |
| `proctree_lock` (`kern/kern_proc.c`) | the process tree, the pid hash, `p_stat`, and wait information | Waiters for children sleep on `p_waitsv` with it. Never held across anything that sleeps for long. |
| `sg_lock`, one per share group (`kern/kern_sproc.c`) | member list/count, resource master copies and versions | Membership changes take `proctree_lock` first. Resource synchronization takes `sg_lock` before `p_lock` and `p_crlock`; release old vnode references after dropping these locks. Group control and detach use `proctree_lock` to keep members alive. No group lock is held across a syscall's resource operation. |
| `fn_lock`, one per pipe (`fs/fifofs/fifo_vnops.c`) | the pipe's buffer, its counts of open ends | Held while data moves to or from the user's buffer, so a fault on it is taken under the lock: it ranks above `a_lock`. Readers sleep on `fn_rsv` and writers on `fn_wsv` with it. After dropping the mutex for notification callbacks, recheck the reader/room predicate under the mutex before entering the writer wait. |
| `sb_lock`, one per LWP semaphore and address-wait hash bucket (`kern/kern_lwp.c`) | changes to semaphore counts in user memory; the bucket's list of address waiters and their woken marks | Held across the copyin and copyout of the count, and across an address waiter's check of its word and its queueing, so it ranks above `a_lock` and a file system's locks. Waiters sleep on the bucket's `sb_sv` with it. |
| `fd_lock`, one per descriptor table (`sys/file.h`) | descriptor slots, descriptor flags, table growth and ownership count | Precedes `p_lock` for descriptor-limit reads and `sd_lock` for STREAMS registration. Drop it before releasing file references or record locks. A process changes its table pointer only before it runs or after making itself single-threaded. |
| `a_brklock`, one per address space (`vm/as.h`) | shared heap base and extent | Precedes `p_lock` and `a_lock`. Copying an address space holds it before reading the layout; changing the break holds it across the segment update. |
| `p_lock`, one per process (`sys/proc.h`) | directories, umask, limits, command name and process blocking counts | May follow `fd_lock`, `sg_lock` or `a_brklock`. Not held while closing a file (`frele` may sleep in a file system). Process blocking sleeps on `p_blocksv` with it and drops it before signal handling. |
| `f_offlock`, one per open file (`sys/file.h`) | the file offset | Held across a read or write through the file, so it ranks above the file system's own locks. |
| `reclock_mutex` (`kern/vfs_reclock.c`) | every vnode's record and flock locks (`v_locks`), the list of lock waiters, the count of locks | Taken by `fcntl`'s lock commands, `flock`, and when a file's locks go: `fd_release` (after `fd_lock` is dropped) and `frele`. `f_offlock` and `VOP_GETATTR`, for a range from the offset or the end, are taken and released before it. May sleep in `kmem_alloc` under it; waiters sleep on `reclock_sv` with it. Takes no other lock. |
| `sd_plumb`, one per stream (`kern/strhead.c`) | pushing, popping and the last close | Taken first. Held while the plumbing claims syncqs (in address order, `sq_claim_set`) and across a module's open and close, which may sleep. |
| `ts_rdlock`, one per TPI socket (`kern/sock_tpi.c`) | a stream socket's one reader (recv, accept, the wait in connect) | Held while waiting for data on `sd_readsv` and across the copy to the user; before `ts_lock`, which accept takes (for the listener's and the new socket's requests) and the indications briefly. |
| `ts_lock`, one per TPI socket (`kern/sock_tpi.c`) | the socket's requests (one at a time) and its bound and connected state | Held across a TPI request: sending it down (claiming the head's syncq) and waiting for its acknowledgement on `sd_readsv` under `sd_lock`, so it ranks above both. Not held across a copy to or from the user, nor while waiting for data. |
| syncq claims (`sq_enter`, `kern/str_queue.c`) | a module's procedures: one thread at a time in a queue pair (or every instance of a module) | Not a lock that procedures wait on: a put to a claimed syncq is left for its holder. Only plumbing (after `sd_plumb`) and the stream head (`str_down`, `str_canput`, holding nothing else) wait to claim one. A holder never sleeps, except plumbing in a module's open or close. Procedures run while it is held may take `sd_lock` (the head's own), so a claim ranks above `sd_lock`. |
| `sd_rdlock`, one per stream head | one reader (read, getmsg) at a time | Before `sd_lock`. Held across the copy to the user, so it ranks above `a_lock`; no stream has pages, so no fault takes it. |
| `sd_iocwait`, one per stream head | one `I_STR` at a time | Held while the ioctl waits for its answer under `sd_iocspin`; takes `sd_lock` briefly and claims the head's syncq. |
| `sd_lock`, one per stream head (`kern/strhead.c`) | signal registrations, the head's flags, errors and options, the read queue's order for readers | May be taken under `fd_lock` for registration and close, and inside the head's syncq by its procedures. Event delivery takes the subscriber's `p_siglock` while holding `sd_lock`. Never held across a copy to or from the user, nor while claiming a syncq. Readers and writers sleep on `sd_readsv` and `sd_writesv` with it. |
| `fmod_lock`, `strdev_lock` (`kern/strhead.c`) | making a per-module syncq | Held only to make or find it. |
| `str_linklock` (`kern/strhead.c`) | the list of multiplexor links | Held only to search, add or take out a link; never across the ioctl to the driver or the plumbing. Linking and unlinking take the lower stream's `sd_plumb` (and its syncqs) after the controlling stream's `sd_iocwait` is dropped, and the controlling stream's `sd_plumb` only briefly, alone. |
| `vnet_lock`, one per virtio-net device (`dev/virtio/vionet.c`) | the device's streams: their list, state, SAP and promiscuous flags | Taken by the streams' procedures (inside their syncqs) and by the device's thread, which holds it while it `put`s frames to the streams: a put never waits, so it may be held across one. |
| `sp_lock`, one per local socket pair (`kern/sockfs.c`) | both receive queues, urgent marks, registered owners, and lifetime | Precedes `proctree_lock` for urgent notification and the poll head lock when waking waiters. Owner validation releases `proctree_lock` before taking `sp_lock`. Read and write prefault the user buffer before taking it. |
| `local_lock` (`kern/sockfs.c`) | bound socket names, listeners, and connection publication | Bind holds it across filesystem node creation. Accept holds it across address copyout and descriptor publication under `p_lock`. Close drops it before releasing endpoint files. |
| `a_lock` (mrlock), one per address space (`vm/as.h`) | the segment list and each segment's range | Faults take it shared, layout changes exclusive. A fault may call `VOP_GETPAGE`, so it ranks above a file system's locks. read and write therefore fault their user buffer in before calling the file system (`uio_prefault`), so the copy inside cannot fault into another file's (or the same file's) `VOP_GETPAGE` with that file system's locks held; mapped pages stay until unmapped, so nothing can undo the prefault meanwhile. |
| `am_lock`, one per anon map (`vm/anon.h`) | the map's anon slots | Held across a fault on the segment, including `VOP_GETPAGE`. |
| `blkdev_lock` (`kern/subr_blkdev.c`) | the block device registry | Held only while registering or looking up. |
| `intr_mutex` (`arch/amd64/amd64/intr.c`) | the interrupt source table and handler lists, for changes | Held across `smp_call_all()` and `delay()` in `intr_disestablish()`; never taken from an interrupt. |

## Spin locks

Listed roughly from outermost to innermost.

| Lock | Protects | Taken after | Notes |
|---|---|---|---|
| `p_crlock`, one per process (`sys/proc.h`) | credential pointer replacement and acquiring its reference | `proctree_lock`, `sg_lock`, `p_lock` | Credentials are immutable once published. Drop it before allocating, freeing or taking the share-group mutex. |
| `p_siglock`, one per process (`sys/proc.h`) | the process's and its LWPs' signal state, the LWP list (`p_lwps`, zombies), every LWP's `l_flags`, and the single-threading and rendezvous state | `proctree_lock` and `sg_lock` (mutexes) may be held | Taken from interrupts (alarm) too. Parked LWPs sleep on `p_rendezsv` with it, and waiters for LWP state changes on `p_lwpsv`. A poster interrupts the LWP's sleep under it, so it ranks above the sync object locks; stopped LWPs sleep on `p_stopsv` with it. Never held while calling `untimeout()`, whose callout takes it. |
| sync object locks: `m_lock`, `sv_lock`, `s_lock`, `mr_lock` (`kern/kern_mutex.c`, `kern_synch.c`, `kern_mrlock.c`) | the object's state and sleep queue | a caller's spin lock (`sv_wait`) | A sleeper drops the object's lock before taking its own `kt_lock`. A waker dequeues under it, then drops it before `wakeup_thread()`, so it never touches the object once the sleeper can run; only the mutex handoff takes the new owner's `kt_lock` under `m_lock`. |
| `bc_lock` (`kern/vfs_bio.c`) | the buffer hash, free list, flags of every buffer, and counters | nothing | Never held across I/O; a buffer is marked busy and the lock dropped first. Waiters sleep on `bc_wait` with `sv_wait()`. |
| `h_lock`, one per user address space (`arch/amd64/amd64/hat.c`) | that space's page tables | nothing | Taken before the node locks (table pages are allocated under it) and so before any `pc_lock`; never held across a TLB flush. |
| `kt_lock`, one per thread (`kern/kern_kthread.c`) | the thread's state, priority, run queue membership, wait channel | sync object locks | Held across a context switch by the thread leaving, and released by the next thread in `sched_finish()`. Never hold two, except the switch handover. |
| `dnlc_lock` (`kern/vfs_dnlc.c`) | the name cache | nothing | Entries are soft references; a hit takes a real one with `vn_get()` under it, so it nests the vnode locks below. |
| `uts_lock` (`kern/kern_syscall.c`) | the names `uname` and `sysinfo` report, the host name | nothing | Held only to copy the names in or out of a kernel buffer; the copy to or from the user is made without it. |
| `pc_lock`, one per vnode (`vm/vm_pvn.c`) | the vnode's page hash, dirty list, and its pages' identity, holds, and `p_vflags` | `hat_klock` (through the allocator's recycling) | Nothing is allocated while it is held. Waiters for a busy page sleep on a hashed `sv` with it as the interlock. |
| `vn_dirty_lock` (`kern/vfs_vnode.c`) | the list of vnodes with dirty pages | `dnlc_lock`, `pc_lock` | The flusher takes vnode references under it. |
| `vf_lock`, one per vnode free list (`kern/vfs_vnode.c`) | that free list | `dnlc_lock`, `vn_dirty_lock` | Never hold two. Dropped before `VOP_RECLAIM` and before tossing pages. |
| `v_lock`, one per vnode (`kern/vfs_vnode.c`) | `v_count`, `v_flag`, `v_number` | `vf_lock` | |
| `vn_all_lock` (`kern/vfs_vnode.c`) | the list of all shared vnodes | nothing | Held only to walk or extend the list. |
| `pvn_ref_lock` (`vm/vm_pvn.c`) | waiting for recyclers to drop `pc_ref` | `pc_lock` is not held | |
| `wt_lock` (`kern/sched_ts.c`) | the time-share active job list and every job's thread list | `kt_lock` | Redistribution holds it and only try-locks threads, leaving a busy thread for the next tick. |
| `rq_lock`, one per processor (`sys/sched.h`) | that processor's run queue | `kt_lock` | Code holding `rq_lock` takes a `kt_lock` only with `spin_trylock()`. Never hold two run queue locks. |
| `vq_lock`, one per virtqueue (`dev/virtio/virtio.c`) | the rings, free list, and cookies | nothing | The completion callback runs under it and may only wake threads (`sema`, `sv` locks nest inside). Submitters wait for space with `sv_wait()` on it. |
| `vnet_rxlock`, one per virtio-net device (`dev/virtio/vionet.c`) | the received buffers waiting for the device's thread | `vq_lock` | Taken by the receive completion, which wakes the thread on `vnet_rxsv`. The transmit completion frees its buffer (`kmem_free`) under `vq_lock`. |
| `sd_qlock`, one per stream (`kern/strhead.c`) | the stream's `q_next` and back links, for walks of more than one queue | nothing | Plumbing changes the links under it, holding the syncqs too. `canputnext` and back-enabling walk under it and take `q_lock`, then `sq_lock`, inside. |
| `q_lock`, one per queue (`kern/str_queue.c`) | the queue's messages, counts, bands and flags | `sd_qlock` | Taken from interrupts (`putq`, `qenable`). May allocate a `qband` (`KM_NOSLEEP`) under it. Released before the syncq is touched. |
| `str_cblock` (`kern/str_queue.c`) | the list of `qtimeout` callbacks | nothing | Held while arming a callout (so `callout_lock` nests inside), and by the firing callout before it takes `sq_lock`. Never held while calling `untimeout()`. |
| `sq_lock`, one per syncq (`kern/str_queue.c`) | the syncq's claim, owner and waiting work | `sd_qlock`, `str_cblock` | Taken from interrupts. Claimants sleep on `sq_sv` with it; `str_runlock` nests inside when it is handed to the service threads. |
| `str_runlock` (`kern/str_queue.c`) | the service threads' list of syncqs with work | `sq_lock` | The service threads sleep on `str_runsv` with it. |
| `sd_iocspin`, one per stream head (`kern/strhead.c`) | the `I_STR` in progress: its id, answer and timeout | nothing | Taken by the head's read procedure and by the timeout's callout; the ioctl sleeps on `sd_iocsv` with it. |
| `pci_conf_lock` (`arch/amd64/amd64/pci_machdep.c`) | the legacy 0xcf8/0xcfc pair; ECAM bus mappings | nothing | |
| `io_lock`, one per I/O APIC (`arch/amd64/amd64/ioapic.c`) | the select/window register pair | nothing | Taken from the hardware interrupt to mask a line. |
| `callout_lock` (`kern/kern_timeout.c`) | the callout list | nothing | Released while a callout function runs. |
| `zombie_lock`, `allthreads_lock` (`kern/kern_kthread.c`) | exited threads; the list of all threads | nothing | |
| `kva_lock` (`vm/vm_kva.c`) | the free KVA range list | nothing | Never held across an allocation; range records are allocated before and freed after. |
| `kz_lock`, one per zone (`kern/kern_kmem.c`) | a zone's slab lists, slabs, and counters | nothing | Never held while calling the page allocator. |
| `kmem_zones_lock` (`kern/kern_kmem.c`) | the list of all zones | nothing | Taken only when a zone is created. |
| `hat_klock` (`arch/amd64/amd64/hat.c`) | the kernel page tables below the PML4, for KVA changes | nothing | Allocates page-table pages, so it is taken before the node locks. Never held across a TLB shootdown. |
| `page_nodes[n].pn_lock` (`vm/vm_page.c`) | node *n*'s free and cache lists, counters, and `p_flags` of its pages | `hat_klock`, `pc_lock` | Never hold two node locks at once; allocation from a remote node drops the local lock first. Recycling a cache page marks it and holds its vnode's cache under this lock, drops it (keeping interrupts off), and only then takes `pc_lock`. |
| `ci_act_lock` (`arch/amd64/amd64/intr.c`) | one processor's action queue | nothing | Held only to queue or dequeue; actions run with it released. Never held while waiting for another processor. |
| `prf_lock` (`kern/subr_prf.c`) | interleaving of `printf` output | any lock | Innermost: `printf` may be called with any lock held. Ignored after a panic. |

## Rules

- Address-space ownership uses an atomic reference count independently of
  `a_lock`. The last owner releases it only after every processor has
  stopped using its page tables.
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
