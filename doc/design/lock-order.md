# Lock Order

Every lock in the kernel, what it protects, and which locks may be held
while taking it. A lock may be taken while holding only the locks listed
before it in its "taken after" column. Update this page in the same
commit that adds or changes a lock.

## Spin locks

| Lock | Protects | Taken after | Notes |
|---|---|---|---|
| `hat_klock` (`arch/amd64/amd64/hat.c`) | the kernel page tables below the PML4, for KVA changes | nothing | Allocates page-table pages, so it is taken before the node locks. Never held across a TLB shootdown. |
| `page_nodes[n].pn_lock` (`vm/vm_page.c`) | node *n*'s free lists, counters, and the free-list fields of its pages | `hat_klock` | Never hold two node locks at once; allocation from a remote node drops the local lock first. |
| `prf_lock` (`kern/subr_prf.c`) | interleaving of `printf` output | any lock | Innermost: `printf` may be called with any lock held. Ignored after a panic. |

## Rules

- Spin locks raise to splhi. Never sleep or block while holding one.
- `prf_lock` is the only lock that may be taken while holding another
  lock of the same rank or deeper. Anything that prints must therefore
  never take another lock from inside the console path.
- After a panic, locks held by a stopped processor are never released.
  Code on the panic path (`panic`, `printf`, `mdboot`) must not wait for
  any lock other than through the `panicstr` bypass.
