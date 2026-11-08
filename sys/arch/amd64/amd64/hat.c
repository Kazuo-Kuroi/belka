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
 * Locking: hat_klock serialises changes to KVA mappings. It is taken
 * before the page allocator's node locks. hat_init() runs before any
 * other processor is started and takes no lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/smp.h>
#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/bootinfo.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/param.h>
#include <machine/pte.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/page.h>

#define KERNEL_PML4_FIRST       256     /* first kernel-half PML4 slot */

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
 * or if a table is missing and cannot be created.
 */
static pt_entry_t *
hat_pte(paddr_t root, vaddr_t va, int create)
{
        pt_entry_t *table = pt_page(root);

        for (int l = PT_LEVELS; l > 1; l--) {
                pt_entry_t *e = &table[PT_INDEX(va, l)];

                if ((*e & PTE_P) == 0) {
                        paddr_t next;

                        if (!create || (next = pt_alloc()) == 0)
                                return NULL;
                        *e = next | PTE_P | PTE_W;
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
