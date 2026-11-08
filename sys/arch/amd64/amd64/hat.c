/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 HAT: the kernel's page tables.
 *
 * hat_init() builds a fresh set of tables on the bootstrap processor:
 *
 * - All 256 kernel-half PML4 entries get their PDPT page at once, so
 *   the PML4's kernel half never changes after boot.
 * - The direct map covers every RAM range in the boot memory map, with
 *   1 GB pages where the processor has them and alignment allows, then
 *   2 MB, then 4 KB. Holes between ranges stay unmapped, so device
 *   memory is never mapped write-back by accident.
 * - The kernel image is mapped page by page at its link address, at the
 *   physical addresses the boot loader used, with text executable and
 *   read-only, read-only data not executable, and data and bss
 *   writable and not executable.
 *
 * Every kernel mapping is global, so it survives address space switches
 * once user processes exist.
 *
 * Page-table pages come from the page allocator and are reached through
 * the direct map. That works both before and after the switch because
 * the direct map keeps the boot loader's base.
 *
 * User address spaces (struct hat) own the lower half of a PML4 of
 * their own, whose upper half is a copy of the kernel's. Their entries
 * are never global, so loading %cr3 flushes them. Each hat records which
 * processors have it loaded, so that removing a translation flushes only
 * those; hat_activate() updates the set as processors switch, with the
 * bit set before %cr3 is loaded and the entries cleared before the set
 * is read, so a processor can never keep a stale translation.
 *
 * Locking: hat_klock serialises changes to KVA mappings, and each hat's
 * h_lock changes to its tables. Both are taken before the page
 * allocator's node locks; allocating a table page may recycle a cached
 * page, so they also come before any vnode's page-cache lock.
 * hat_init() runs before any other processor is started and takes no
 * lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/smp.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/cpu.h>

#include <machine/bootinfo.h>
#include <machine/cpufunc.h>
#include <machine/intr.h>
#include <machine/md_var.h>
#include <machine/param.h>
#include <machine/pte.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/page.h>

#define KERNEL_PML4_FIRST       256     /* first kernel-half PML4 slot */
#define HAT_NCPUWORDS           ((MAXCPUS + 63) / 64)
#define HAT_UNLOAD_BATCH        32

struct hat {
        paddr_t         h_pml4;
        struct spinlock h_lock;
        volatile uint64_t h_cpus[HAT_NCPUWORDS]; /* processors using it */
};

static paddr_t kernel_pml4;             /* physical address of the PML4 */
static pt_entry_t pte_nx;               /* PTE_NX, or 0 without NX */
static int have_1gb;
static struct spinlock hat_klock = SPINLOCK_INITIALIZER("hat_k");

/* Counters for the boot report. */
static size_t hat_dmap_pages[4];        /* mappings at levels 1 to 3 */

static pt_entry_t *pt_page(paddr_t);
static paddr_t pt_alloc(void);
static int hat_map(paddr_t, vaddr_t, paddr_t, int, pt_entry_t);
static int hat_lookup(paddr_t, vaddr_t, paddr_t *, pt_entry_t *);
static pt_entry_t *hat_pte(paddr_t, vaddr_t, int);
static void hat_dmap_range(paddr_t, paddr_t);
static void hat_map_kernel(void);
static void hat_cpu_features(void);
static void hat_shootdown(void *);

static pt_entry_t *
pt_page(paddr_t pa)
{
        return phys_to_kva(pa);
}

/* Allocate a zeroed page-table page. */
static paddr_t
pt_alloc(void)
{
        struct page *pp;

        if ((pp = page_alloc(0, PA_ZERO)) == NULL)
                return 0;
        return page_to_pa(pp);
}

/*
 * Enter a mapping va -> pa of the size of level (1 for 4 KB, 2 for
 * 2 MB, 3 for 1 GB) in the tables rooted at root, creating intermediate
 * tables as needed. bits are the leaf PTE bits apart from the frame and
 * PTE_PS. Returns 0, -1 if something is already mapped there, or -2 if a
 * table page cannot be allocated.
 */
static int
hat_map(paddr_t root, vaddr_t va, paddr_t pa, int level, pt_entry_t bits)
{
        pt_entry_t *table = pt_page(root);

        for (int l = PT_LEVELS; l > level; l--) {
                pt_entry_t *e = &table[PT_INDEX(va, l)];

                if ((*e & PTE_P) == 0) {
                        paddr_t next = pt_alloc();

                        if (next == 0)
                                return -2;
                        /* Upper levels allow everything; leaves restrict. */
                        *e = next | PTE_P | PTE_W;
                } else if ((*e & PTE_PS) != 0) {
                        return -1;
                }
                table = pt_page(*e & PTE_FRAME);
        }

        table += PT_INDEX(va, level);
        if ((*table & PTE_P) != 0)
                return -1;
        *table = pa | bits | PTE_P | (level > 1 ? PTE_PS : 0);
        return 0;
}

/*
 * Translate va through the tables rooted at root. On success store the
 * physical address and the leaf entry and return the leaf's level;
 * return -1 if va is unmapped.
 */
static int
hat_lookup(paddr_t root, vaddr_t va, paddr_t *pap, pt_entry_t *ptep)
{
        pt_entry_t *table = pt_page(root);

        for (int l = PT_LEVELS; l >= 1; l--) {
                pt_entry_t e = table[PT_INDEX(va, l)];

                if ((e & PTE_P) == 0)
                        return -1;
                if (l == 1 || (l <= 3 && (e & PTE_PS) != 0)) {
                        paddr_t frame = e & (l == 1 ? PTE_FRAME : PTE_LGFRAME);

                        frame &= ~(PT_SIZE(l) - 1);
                        if (pap != NULL)
                                *pap = frame | (va & (PT_SIZE(l) - 1));
                        if (ptep != NULL)
                                *ptep = e;
                        return l;
                }
                table = pt_page(e & PTE_FRAME);
        }
        return -1;
}

/*
 * Return the level-1 entry for va in the tables rooted at root, creating
 * page tables if create is set. Returns NULL if va lies in a large page,
 * or if a table is missing and cannot be created. Tables created for a
 * user address space allow user access at every level (PTE_U); the
 * leaves restrict it.
 */
static pt_entry_t *
hat_pte(paddr_t root, vaddr_t va, int create)
{
        pt_entry_t ubit = va < VM_USER_MAX ? PTE_U : 0;
        pt_entry_t *table = pt_page(root);

        for (int l = PT_LEVELS; l > 1; l--) {
                pt_entry_t *e = &table[PT_INDEX(va, l)];

                if ((*e & PTE_P) == 0) {
                        paddr_t next;

                        if (!create || (next = pt_alloc()) == 0)
                                return NULL;
                        *e = next | PTE_P | PTE_W | ubit;
                } else if ((*e & PTE_PS) != 0) {
                        return NULL;
                }
                table = pt_page(*e & PTE_FRAME);
        }
        return &table[PT_INDEX(va, 1)];
}

/* Direct-map physical [start, end) with the largest pages that fit. */
static void
hat_dmap_range(paddr_t start, paddr_t end)
{
        pt_entry_t bits = PTE_W | PTE_G | pte_nx;
        paddr_t pa = start;

        while (pa < end) {
                int level = 1;

                if (have_1gb && (pa & (NBPD_1G - 1)) == 0 &&
                    end - pa >= NBPD_1G)
                        level = 3;
                else if ((pa & (NBPD_2M - 1)) == 0 && end - pa >= NBPD_2M)
                        level = 2;
                if (hat_map(kernel_pml4, dmap_base + pa, pa, level, bits) != 0)
                        panic("hat: cannot direct-map %#jx", (uintmax_t)pa);
                hat_dmap_pages[level]++;
                pa += PT_SIZE(level);
        }
}

/*
 * Map the kernel image. The physical address of each page is read from
 * the boot loader's tables, which are still live, so nothing is assumed
 * about how the image was laid out in physical memory.
 */
static void
hat_map_kernel(void)
{
        paddr_t loader_root = rcr3() & PTE_FRAME;
        vaddr_t va;

        for (va = (vaddr_t)(uintptr_t)kernel_start;
            va < (vaddr_t)(uintptr_t)kernel_end; va += PAGESIZE) {
                pt_entry_t bits;
                paddr_t pa;

                if (hat_lookup(loader_root, va, &pa, NULL) < 0)
                        panic("hat: kernel page %#jx is not mapped",
                            (uintmax_t)va);
                if (va < (vaddr_t)(uintptr_t)etext)
                        bits = PTE_G;                   /* read, execute */
                else if (va < (vaddr_t)(uintptr_t)erodata)
                        bits = PTE_G | pte_nx;          /* read */
                else
                        bits = PTE_G | PTE_W | pte_nx;  /* read, write */
                if (hat_map(kernel_pml4, va, pa & PTE_FRAME, 1, bits) != 0)
                        panic("hat: cannot map kernel page %#jx",
                            (uintmax_t)va);
        }
}

/* Record which paging features the bootstrap processor has. */
static void
hat_cpu_features(void)
{
        uint32_t regs[4];

        cpuid(0x80000000, 0, regs);
        if (regs[0] < 0x80000001)
                return;
        cpuid(0x80000001, 0, regs);
        if ((regs[3] & CPUID_EXT_NX) != 0)
                pte_nx = PTE_NX;
        if ((regs[3] & CPUID_EXT_1GB) != 0)
                have_1gb = 1;
}

void
hat_init(void)
{
        const struct bootinfo *bi = &bootinfo;
        pt_entry_t *pml4;
        paddr_t max_pa = 0;

        hat_cpu_features();
        if ((dmap_base & (NBPD_2M - 1)) != 0)
                panic("hat_init: direct map base %#jx is not 2 MB aligned",
                    (uintmax_t)dmap_base);
        if ((dmap_base & (NBPD_1G - 1)) != 0)
                have_1gb = 0;

        if ((kernel_pml4 = pt_alloc()) == 0)
                panic("hat_init: no memory for the PML4");
        pml4 = pt_page(kernel_pml4);
        for (int i = KERNEL_PML4_FIRST; i < PT_ENTRIES; i++) {
                paddr_t pdpt = pt_alloc();

                if (pdpt == 0)
                        panic("hat_init: no memory for kernel PDPTs");
                pml4[i] = pdpt | PTE_P | PTE_W;
        }

        /*
         * Direct-map RAM: everything the firmware or loader reports as
         * memory rather than as device space. The Limine protocol gives
         * the map sorted and without overlaps; adjacent RAM ranges are
         * joined so that large pages can span them.
         */
        for (uint32_t i = 0; i < bi->bi_nmem; ) {
                paddr_t start, end;
                uint32_t j;

                if (!bootmem_is_ram(bi->bi_mem[i].bm_type)) {
                        i++;
                        continue;
                }
                start = bi->bi_mem[i].bm_base;
                end = start + bi->bi_mem[i].bm_length;
                for (j = i + 1; j < bi->bi_nmem &&
                    bootmem_is_ram(bi->bi_mem[j].bm_type) &&
                    bi->bi_mem[j].bm_base == end; j++)
                        end += bi->bi_mem[j].bm_length;
                start = ptrunc(start);
                end = pround(end);
                if (end > max_pa)
                        max_pa = end;
                if (max_pa > DMAP_MAXSIZE)
                        panic("hat_init: memory above %#jx is not supported",
                            (uintmax_t)DMAP_MAXSIZE);
                hat_dmap_range(start, end);
                i = j;
        }
        if (dmap_base + DMAP_MAXSIZE > KVA_BASE || dmap_base < VM_KERNEL_LOW)
                panic("hat_init: direct map at %#jx overlaps the layout",
                    (uintmax_t)dmap_base);

        hat_map_kernel();
        hat_cpu_init();

        printf("hat: direct map to %#jx: %zu 1G, %zu 2M, %zu 4K pages; "
            "NX %s\n", (uintmax_t)max_pa, hat_dmap_pages[3],
            hat_dmap_pages[2], hat_dmap_pages[1],
            pte_nx != 0 ? "on" : "unavailable");
}

/*
 * Prepare the calling processor and switch it to the kernel tables:
 * enable NX if available, program the PAT, load CR3, and turn on global
 * pages. Interrupts must be disabled.
 */
void
hat_cpu_init(void)
{
        if (pte_nx != 0)
                wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
        wrmsr(MSR_PAT, PAT_VALUE);
        lcr3(kernel_pml4);
        /* Setting PGE also flushes every TLB entry, global or not. */
        lcr4(rcr4() | CR4_PGE);
}

int
hat_nx_enabled(void)
{
        return pte_nx != 0;
}

int
hat_kextract(vaddr_t va, paddr_t *pap)
{
        return hat_lookup(kernel_pml4, va, pap, NULL) < 0 ? -1 : 0;
}

int
hat_kenter(vaddr_t va, paddr_t pa, unsigned int attr)
{
        pt_entry_t bits = PTE_P | PTE_G;
        pt_entry_t *pte;
        int s;
        int error = 0;

        if (va < KVA_BASE || va >= KVA_END || (va & PAGEOFFSET) != 0 ||
            (pa & PAGEOFFSET) != 0)
                panic("hat_kenter: bad mapping %#jx -> %#jx",
                    (uintmax_t)va, (uintmax_t)pa);

        if ((attr & HAT_WRITE) != 0)
                bits |= PTE_W;
        if ((attr & HAT_EXEC) == 0)
                bits |= pte_nx;
        if ((attr & HAT_NOCACHE) != 0)
                bits |= PTE_PCD | PTE_PWT;              /* PAT index 3 */
        else if ((attr & HAT_WCOMBINE) != 0)
                bits |= PTE_PAT4K | PTE_PWT;            /* PAT index 5 */

        s = spin_lock(&hat_klock);
        if ((pte = hat_pte(kernel_pml4, va, 1)) == NULL ||
            (*pte & PTE_P) != 0)
                error = -1;
        else
                *pte = pa | bits;
        spin_unlock(&hat_klock, s);

        /*
         * A new mapping needs no TLB flush: amd64 does not cache
         * not-present entries.
         */
        return error;
}

struct shootdown {
        vaddr_t         sd_va;
        size_t          sd_npages;
};

static void
hat_shootdown(void *arg)
{
        const struct shootdown *sd = arg;

        for (size_t i = 0; i < sd->sd_npages; i++)
                invlpg(sd->sd_va + ptob(i));
}

/*
 * Remove KVA mappings, then flush them on every processor before
 * returning, so no processor can use them afterwards.
 *
 * The flush uses smp_call_all(), so for now this may only be called
 * from outside another smp_call_all() and with no spin lock held.
 * Inter-processor interrupts will lift that restriction.
 */
void
hat_kremove(vaddr_t va, size_t npages)
{
        struct shootdown sd;
        int s;

        s = spin_lock(&hat_klock);
        for (size_t i = 0; i < npages; i++) {
                pt_entry_t *pte = hat_pte(kernel_pml4, va + ptob(i), 0);

                if (pte != NULL)
                        *pte = 0;
        }
        spin_unlock(&hat_klock, s);

        sd.sd_va = va;
        sd.sd_npages = npages;
        if (ncpus > 1)
                smp_call_all(hat_shootdown, &sd);
        else
                hat_shootdown(&sd);
}

/* --- User address spaces. --- */

static struct kmem_zone *hat_zone;

/* An empty user address space, sharing the kernel half. */
struct hat *
hat_alloc(void)
{
        struct hat *hat;
        pt_entry_t *pml4, *kpml4 = pt_page(kernel_pml4);

        if (hat_zone == NULL)
                hat_zone = kmem_zone_init(sizeof(struct hat), "hat");
        hat = kmem_zone_alloc(hat_zone, KM_SLEEP);
        memset(hat, 0, sizeof(*hat));
        spinlock_init(&hat->h_lock, "hat");
        if ((hat->h_pml4 = pt_alloc()) == 0)
                panic("hat_alloc: out of memory");
        pml4 = pt_page(hat->h_pml4);
        for (int i = KERNEL_PML4_FIRST; i < PT_ENTRIES; i++)
                pml4[i] = kpml4[i];
        return hat;
}

/* Free the table pages below a level-l table, then the table itself. */
static void
hat_free_table(paddr_t table, int level)
{
        pt_entry_t *t = pt_page(table);

        if (level > 1) {
                for (int i = 0; i < PT_ENTRIES; i++) {
                        if ((t[i] & PTE_P) != 0)
                                hat_free_table(t[i] & PTE_FRAME, level - 1);
                }
        }
        page_free(pa_to_page(table), 0);
}

/*
 * Free a hat and its user tables. No processor may have it loaded, and
 * its translations must already be unloaded (the pages they named are
 * the caller's).
 */
void
hat_free(struct hat *hat)
{
        pt_entry_t *pml4 = pt_page(hat->h_pml4);

        for (int w = 0; w < HAT_NCPUWORDS; w++) {
                if (hat->h_cpus[w] != 0)
                        panic("hat_free: hat %p still loaded", (void *)hat);
        }
        for (int i = 0; i < KERNEL_PML4_FIRST; i++) {
                if ((pml4[i] & PTE_P) != 0)
                        hat_free_table(pml4[i] & PTE_FRAME, PT_LEVELS - 1);
        }
        page_free(pa_to_page(hat->h_pml4), 0);
        kmem_zone_free(hat_zone, hat);
}

/*
 * Make hat the user address space of this processor (NULL for none),
 * as the scheduler switches threads. Interrupts are disabled.
 */
void
hat_activate(struct hat *hat)
{
        struct cpu_info *ci = curcpu();
        uint64_t bit = UINT64_C(1) << (ci->ci_cpuid % 64);
        unsigned int w = ci->ci_cpuid / 64;
        paddr_t want = hat != NULL ? hat->h_pml4 : kernel_pml4;
        struct hat *old = ci->ci_hat;

        if (old == hat)
                return;
        if (hat != NULL)
                atomic_or_64(&hat->h_cpus[w], bit);     /* before %cr3 */
        lcr3(want);
        if (old != NULL)
                atomic_and_64(&old->h_cpus[w], ~bit);
        ci->ci_hat = hat;
}

static pt_entry_t
hat_user_bits(unsigned int attr)
{
        pt_entry_t bits = PTE_P | PTE_U;

        if ((attr & HAT_WRITE) != 0)
                bits |= PTE_W;
        if ((attr & HAT_EXEC) == 0)
                bits |= pte_nx;
        return bits;
}

/*
 * Map the page at pa at user address va. Returns 0, -1 if va is
 * already mapped, or -2 if a table page cannot be allocated.
 */
int
hat_memload(struct hat *hat, vaddr_t va, paddr_t pa, unsigned int attr)
{
        pt_entry_t *pte;
        int s, error = 0;

        if (va >= VM_USER_MAX || (va & PAGEOFFSET) != 0 ||
            (pa & PAGEOFFSET) != 0)
                panic("hat_memload: bad mapping %#jx -> %#jx",
                    (uintmax_t)va, (uintmax_t)pa);
        s = spin_lock(&hat->h_lock);
        if ((pte = hat_pte(hat->h_pml4, va, 1)) == NULL)
                error = -2;
        else if ((*pte & PTE_P) != 0)
                error = -1;
        else
                *pte = pa | hat_user_bits(attr);
        spin_unlock(&hat->h_lock, s);
        return error;
}

struct hat_flush {
        paddr_t         hf_pml4;
        const vaddr_t   *hf_va;
        size_t          hf_n;
};

static void
hat_flush_action(void *arg)
{
        const struct hat_flush *hf = arg;

        if ((rcr3() & PTE_FRAME) != hf->hf_pml4)
                return;
        for (size_t i = 0; i < hf->hf_n; i++)
                invlpg(hf->hf_va[i]);
}

/*
 * Flush n translations of hat from the processors that have it loaded.
 * The entries have been changed already; no spin lock may be held.
 */
static void
hat_flush(struct hat *hat, const vaddr_t *va, size_t n)
{
        struct hat_flush hf = { hat->h_pml4, va, n };
        struct cpu_info *ci;
        int others = 0, s;

        if (n == 0)
                return;
        membar_sync();          /* the entries before the processor set */
        s = splhi();
        ci = curcpu();
        for (int w = 0; w < HAT_NCPUWORDS; w++) {
                uint64_t mine = (unsigned int)w == ci->ci_cpuid / 64 ?
                    UINT64_C(1) << (ci->ci_cpuid % 64) : 0;

                if ((hat->h_cpus[w] & ~mine) != 0)
                        others = 1;
        }
        if (!others) {
                hat_flush_action(&hf);
                splx(s);
                return;
        }
        splx(s);
        smp_call_all(hat_flush_action, &hf);
}

/*
 * Remove the translations of npages pages from va. For each one that
 * was there, fn(arg, va, pa) is called once it is flushed everywhere.
 */
void
hat_unload(struct hat *hat, vaddr_t va, size_t npages,
    void (*fn)(void *, vaddr_t, paddr_t), void *arg)
{
        vaddr_t end = va + ptob(npages);

        while (va < end) {
                vaddr_t vas[HAT_UNLOAD_BATCH];
                paddr_t pas[HAT_UNLOAD_BATCH];
                size_t n = 0;
                int s = spin_lock(&hat->h_lock);

                while (va < end && n < HAT_UNLOAD_BATCH) {
                        pt_entry_t *pte = hat_pte(hat->h_pml4, va, 0);

                        if (pte == NULL) {
                                /* No level-1 table: skip all it covers. */
                                vaddr_t next = (va + NBPD_2M) &
                                    ~(vaddr_t)(NBPD_2M - 1);

                                va = next < end && next > va ? next : end;
                                continue;
                        }
                        if ((*pte & PTE_P) != 0) {
                                vas[n] = va;
                                pas[n] = *pte & PTE_FRAME;
                                *pte = 0;
                                n++;
                        }
                        va += PAGESIZE;
                }
                spin_unlock(&hat->h_lock, s);
                hat_flush(hat, vas, n);
                if (fn != NULL) {
                        for (size_t i = 0; i < n; i++)
                                fn(arg, vas[i], pas[i]);
                }
        }
}

/*
 * Set the access of the present translations of npages pages from va to
 * attr (HAT_WRITE, HAT_EXEC), flushing them.
 */
void
hat_chgprot(struct hat *hat, vaddr_t va, size_t npages, unsigned int attr)
{
        vaddr_t end = va + ptob(npages);
        pt_entry_t bits = hat_user_bits(attr);

        while (va < end) {
                vaddr_t vas[HAT_UNLOAD_BATCH];
                size_t n = 0;
                int s = spin_lock(&hat->h_lock);

                while (va < end && n < HAT_UNLOAD_BATCH) {
                        pt_entry_t *pte = hat_pte(hat->h_pml4, va, 0);

                        if (pte == NULL) {
                                vaddr_t next = (va + NBPD_2M) &
                                    ~(vaddr_t)(NBPD_2M - 1);

                                va = next < end && next > va ? next : end;
                                continue;
                        }
                        if ((*pte & PTE_P) != 0 &&
                            (*pte & (PTE_W | PTE_NX | PTE_U | PTE_P)) !=
                            bits) {
                                *pte = (*pte & PTE_FRAME) | bits;
                                vas[n++] = va;
                        }
                        va += PAGESIZE;
                }
                spin_unlock(&hat->h_lock, s);
                hat_flush(hat, vas, n);
        }
}

/*
 * Whether va is mapped in hat: 1 with its page and access, or 0. The
 * answer may be out of date unless the caller excludes changes.
 */
int
hat_probe(struct hat *hat, vaddr_t va, paddr_t *pap, unsigned int *attrp)
{
        pt_entry_t *pte, e;
        int s = spin_lock(&hat->h_lock);

        pte = hat_pte(hat->h_pml4, ptrunc(va), 0);
        e = pte != NULL ? *pte : 0;
        spin_unlock(&hat->h_lock, s);
        if ((e & PTE_P) == 0)
                return 0;
        if (pap != NULL)
                *pap = e & PTE_FRAME;
        if (attrp != NULL)
                *attrp = ((e & PTE_W) != 0 ? HAT_WRITE : 0) |
                    ((e & PTE_NX) == 0 ? HAT_EXEC : 0);
        return 1;
}
