/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * User floating-point state. See <machine/fpu.h>.
 *
 * fpu_init() runs once on the bootstrap processor: it decides between
 * XSAVE and FXSAVE, enables the state components the processor has
 * (x87, SSE, AVX), and records a clean initial state, which every new
 * image starts from. fpu_cpu_init() then sets each processor up the
 * same way.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/systm.h>
#include <sys/ucontext.h>

#include <machine/cpufunc.h>
#include <machine/fpu.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/page.h>

#define CR0_MP          0x00000002      /* monitor coprocessor */
#define CR0_EM          0x00000004      /* emulate: must be clear */
#define CR0_TS          0x00000008      /* task switched: lazy, unused */
#define CR4_OSFXSR      0x00000200
#define CR4_OSXMMEXCPT  0x00000400
#define CR4_OSXSAVE     0x00040000

#define CPUID1_ECX_XSAVE 0x04000000
#define CPUID1_ECX_AVX  0x10000000

#define XCR0_X87        0x0001
#define XCR0_SSE        0x0002
#define XCR0_AVX        0x0004

#define MXCSR_DEFAULT   0x1f80          /* all exceptions masked */

static int fpu_use_xsave;
static uint64_t fpu_xcr0;
static size_t fpu_size = 512;           /* FXSAVE's area */
static void *fpu_template;              /* the initial state */
static uint32_t fpu_mxcsr_mask = 0xffbf;        /* MXCSR bits allowed */

void
fpu_cpu_init(void)
{
        uint64_t cr4 = rcr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;

        lcr0((rcr0() | CR0_MP) & ~(uint64_t)(CR0_EM | CR0_TS));
        if (fpu_use_xsave)
                cr4 |= CR4_OSXSAVE;
        lcr4(cr4);
        if (fpu_use_xsave)
                fpu_xsetbv(0, fpu_xcr0);
}

void
fpu_init(void)
{
        static const uint32_t mxcsr = MXCSR_DEFAULT;
        uint32_t regs[4];

        cpuid(1, 0, regs);
        if ((regs[2] & CPUID1_ECX_XSAVE) != 0) {
                fpu_use_xsave = 1;
                fpu_xcr0 = XCR0_X87 | XCR0_SSE;
                if ((regs[2] & CPUID1_ECX_AVX) != 0)
                        fpu_xcr0 |= XCR0_AVX;
        }
        fpu_cpu_init();
        if (fpu_use_xsave) {
                /* With XCR0 set, leaf 0xd gives the area it needs. */
                cpuid(0xd, 0, regs);
                fpu_size = regs[1];
        }
        /* It must fit a page, and a ucontext_t's fpregs. */
        if (fpu_size > PAGESIZE || fpu_size > FPREGS_SIZE)
                panic("fpu_init: save area of %zu bytes", fpu_size);

        fpu_template = fpu_area_alloc();
        fpu_fninit();
        fpu_ldmxcsr(&mxcsr);
        fpu_save(fpu_template);
        /* The FXSAVE image says which MXCSR bits may be set (offset 28). */
        {
                uint32_t mask;

                memcpy(&mask, (char *)fpu_template + 28, sizeof(mask));
                if (mask != 0)
                        fpu_mxcsr_mask = mask;
        }
        printf("fpu: %s, %zu-byte save area%s\n",
            fpu_use_xsave ? "xsave" : "fxsave", fpu_size,
            (fpu_xcr0 & XCR0_AVX) != 0 ? ", AVX" : "");
}

/*
 * Make a save area from user space safe to load: an MXCSR or an XSAVE
 * header the processor rejects would fault in the kernel. Only bits for
 * enabled state components survive, in the standard (uncompacted) form.
 */
void
fpu_sanitize(void *area)
{
        char *a = area;
        uint32_t mxcsr;

        memcpy(&mxcsr, a + 24, sizeof(mxcsr));
        mxcsr &= fpu_mxcsr_mask;
        memcpy(a + 24, &mxcsr, sizeof(mxcsr));
        if (fpu_use_xsave) {
                uint64_t bv;

                memcpy(&bv, a + 512, sizeof(bv));
                bv &= fpu_xcr0;
                memcpy(a + 512, &bv, sizeof(bv));
                memset(a + 520, 0, 56);         /* XCOMP_BV, reserved */
        }
}

size_t
fpu_area_size(void)
{
        return fpu_size;
}

/* A zeroed, page-aligned save area, which both instructions need. */
void *
fpu_area_alloc(void)
{
        struct page *pp = page_alloc(0, PA_ZERO);

        if (pp == NULL)
                panic("fpu_area_alloc: out of memory");
        return page_to_kva(pp);
}

void
fpu_area_free(void *area)
{
        page_free(pa_to_page(kva_to_phys(area)), 0);
}

/* The state a new program starts with. */
void
fpu_area_reset(void *area)
{
        memcpy(area, fpu_template, PAGESIZE);
}

void
fpu_save(void *area)
{
        if (fpu_use_xsave)
                fpu_xsave(area, fpu_xcr0);
        else
                fpu_fxsave(area);
}

void
fpu_restore(void *area)
{
        if (fpu_use_xsave)
                fpu_xrstor(area, fpu_xcr0);
        else
                fpu_fxrstor(area);
}
