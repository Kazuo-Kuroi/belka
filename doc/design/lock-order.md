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
| `intr_mutex` (`arch/amd64/amd64/intr.c`) | the interrupt source table and handler lists, for changes | Held across `smp_call_all()` and `delay()` in `intr_disestablish()`; never taken from an interrupt. |

## Spin locks

Listed roughly from outermost to innermost.

| Lock | Protects | Taken after | Notes |
|---|---|---|---|
| sync object locks: `m_lock`, `sv_lock`, `s_lock`, `mr_lock` (`kern/kern_mutex.c`, `kern_synch.c`, `kern_mrlock.c`) | the object's state and sleep queue | a caller's spin lock (`sv_wait`) | A sleeper drops the object's lock before taking its own `kt_lock`; a waker takes `kt_lock` under it. |
| `kt_lock`, one per thread (`kern/kern_kthread.c`) | the thread's state, priority, run queue membership, wait channel | sync object locks | Held across a context switch by the thread leaving, and released by the next thread in `sched_finish()`. Never hold two, except the switch handover. |
| `rq_lock`, one per processor (`sys/sched.h`) | that processor's run queue | `kt_lock` | Code holding `rq_lock` takes a `kt_lock` only with `spin_trylock()`. Never hold two run queue locks. |
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
