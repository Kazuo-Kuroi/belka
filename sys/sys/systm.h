/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Kernel-wide declarations: formatted output, panic, the string
 * routines in libkern, and the hooks between machine-independent and
 * machine-dependent startup.
 */

#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/compiler.h>

/* subr_prf.c */
int printf(const char *, ...) __printflike(1, 2);
int vprintf(const char *, va_list) __printflike(1, 0);
int snprintf(char *, size_t, const char *, ...) __printflike(3, 4);
int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
void panic(const char *, ...) __dead __printflike(1, 2);
extern const char *volatile panicstr;  /* format of the first panic */

/* libkern/string.c */
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);

/* kern_clock.c */
extern volatile uint64_t lbolt;         /* clock ticks since start */
void clock(void);

/*
 * Machine-dependent: move the bootstrap processor's exception stacks to
 * guarded KVA stacks, once kernel stacks can be allocated.
 */
void md_stacks_init(void);

/*
 * Machine-dependent: set up interrupt controllers and start the clock on
 * the bootstrap processor, leaving interrupts disabled.
 */
void intr_init(void);

/* Busy-wait for at least the given number of microseconds. */
void drv_usecwait(unsigned long);

/* init_main.c: machine-independent startup, entered from MD code. */
void main(void) __dead;

/*
 * Machine-dependent: release memory held for the boot loader, once all
 * processors run on the kernel's own tables and stacks.
 */
void bootloader_reclaim(void);

/* subr_selftest.c */
int selftest_run(void);

/* Machine-dependent self-tests; returns the number of failed checks. */
int md_selftest(void);

/*
 * Machine-dependent shutdown. fcn is one of the AD_* values in
 * <sys/uadmin.h>; status is 0 for a clean stop and nonzero after a
 * failure, and is reported to a test harness when one is attached.
 */
void mdboot(int, int) __dead;

#endif /* _SYS_SYSTM_H_ */
