/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Floating-point and vector state of user LWPs.
 *
 * The kernel is built without x87, SSE, or AVX code, so the registers
 * only ever hold user state. Each LWP has a save area of its own, one
 * page, and the state is switched eagerly: saved when an LWP's thread
 * leaves the processor and restored when one arrives. XSAVE is used
 * where the processor has it, for x87, SSE, and AVX; FXSAVE otherwise.
 */

#ifndef _MACHINE_FPU_H_
#define _MACHINE_FPU_H_

#include <stddef.h>
#include <stdint.h>

void fpu_init(void);
void fpu_cpu_init(void);
void *fpu_area_alloc(void);
void fpu_area_free(void *);
void fpu_area_reset(void *);
void fpu_area_sproc(void *);
void fpu_save(void *);
void fpu_restore(void *);
void fpu_sanitize(void *);
size_t fpu_area_size(void);

/* fpusubr.S */
void fpu_fxsave(void *);
void fpu_fxrstor(void *);
void fpu_xsave(void *, uint64_t);
void fpu_xrstor(void *, uint64_t);
void fpu_xsetbv(uint32_t, uint64_t);
void fpu_fninit(void);
void fpu_ldmxcsr(const uint32_t *);
void lcr0(uint64_t);

#endif /* _MACHINE_FPU_H_ */
