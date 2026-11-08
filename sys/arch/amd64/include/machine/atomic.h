/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Atomic memory operations and memory barriers for amd64.
 *
 * The interface follows the atomic_ops(3) naming used by NetBSD and
 * Solaris. Compare-and-swap and swap return the previous value. The
 * "_nv" forms return the new value. Every read-modify-write operation
 * is a full barrier on amd64 because it uses a locked instruction.
 *
 * Each operation exists out of line in atomic.S. When the compiler
 * supports inline assembly, the inline versions below are used instead.
 *
 * Memory barriers:
 *   membar_enter()     after acquiring a lock
 *   membar_exit()      before releasing a lock
 *   membar_producer()  orders stores before later stores
 *   membar_consumer()  orders loads before later loads
 *   membar_sync()      orders all earlier accesses before all later ones
 *
 * amd64 already orders loads with loads, stores with stores, and loads
 * before stores, so only membar_sync() needs an instruction. The others
 * only stop the compiler from reordering.
 */

#ifndef _MACHINE_ATOMIC_H_
#define _MACHINE_ATOMIC_H_

#include <stdint.h>

#include <sys/compiler.h>

#ifdef HAVE_INLINE_ASM

static inline uint32_t
atomic_cas_32(volatile uint32_t *p, uint32_t expected, uint32_t new)
{
        uint32_t old;

        __asm__ __volatile__("lock; cmpxchgl %2, %1"
            : "=a"(old), "+m"(*p)
            : "r"(new), "0"(expected)
            : "memory", "cc");
        return old;
}

static inline uint64_t
atomic_cas_64(volatile uint64_t *p, uint64_t expected, uint64_t new)
{
        uint64_t old;

        __asm__ __volatile__("lock; cmpxchgq %2, %1"
            : "=a"(old), "+m"(*p)
            : "r"(new), "0"(expected)
            : "memory", "cc");
        return old;
}

static inline uint32_t
atomic_swap_32(volatile uint32_t *p, uint32_t new)
{
        __asm__ __volatile__("xchgl %0, %1"
            : "+r"(new), "+m"(*p)
            :
            : "memory");
        return new;
}

static inline uint64_t
atomic_swap_64(volatile uint64_t *p, uint64_t new)
{
        __asm__ __volatile__("xchgq %0, %1"
            : "+r"(new), "+m"(*p)
            :
            : "memory");
        return new;
}

static inline uint32_t
atomic_add_32_nv(volatile uint32_t *p, int32_t delta)
{
        uint32_t old = (uint32_t)delta;

        __asm__ __volatile__("lock; xaddl %0, %1"
            : "+r"(old), "+m"(*p)
            :
            : "memory", "cc");
        return old + (uint32_t)delta;
}

static inline uint64_t
atomic_add_64_nv(volatile uint64_t *p, int64_t delta)
{
        uint64_t old = (uint64_t)delta;

        __asm__ __volatile__("lock; xaddq %0, %1"
            : "+r"(old), "+m"(*p)
            :
            : "memory", "cc");
        return old + (uint64_t)delta;
}

static inline void
atomic_or_32(volatile uint32_t *p, uint32_t bits)
{
        __asm__ __volatile__("lock; orl %1, %0"
            : "+m"(*p)
            : "r"(bits)
            : "memory", "cc");
}

static inline void
atomic_and_32(volatile uint32_t *p, uint32_t bits)
{
        __asm__ __volatile__("lock; andl %1, %0"
            : "+m"(*p)
            : "r"(bits)
            : "memory", "cc");
}

static inline void
atomic_or_64(volatile uint64_t *p, uint64_t bits)
{
        __asm__ __volatile__("lock; orq %1, %0"
            : "+m"(*p)
            : "r"(bits)
            : "memory", "cc");
}

static inline void
atomic_and_64(volatile uint64_t *p, uint64_t bits)
{
        __asm__ __volatile__("lock; andq %1, %0"
            : "+m"(*p)
            : "r"(bits)
            : "memory", "cc");
}

static inline void
membar_sync(void)
{
        __asm__ __volatile__("lock; addq $0, (%%rsp)" : : : "memory", "cc");
}

#define membar_enter()          __insn_barrier()
#define membar_exit()           __insn_barrier()
#define membar_producer()       __insn_barrier()
#define membar_consumer()       __insn_barrier()

#else /* !HAVE_INLINE_ASM */

uint32_t atomic_cas_32(volatile uint32_t *, uint32_t, uint32_t);
uint64_t atomic_cas_64(volatile uint64_t *, uint64_t, uint64_t);
uint32_t atomic_swap_32(volatile uint32_t *, uint32_t);
uint64_t atomic_swap_64(volatile uint64_t *, uint64_t);
uint32_t atomic_add_32_nv(volatile uint32_t *, int32_t);
uint64_t atomic_add_64_nv(volatile uint64_t *, int64_t);
void atomic_or_32(volatile uint32_t *, uint32_t);
void atomic_and_32(volatile uint32_t *, uint32_t);
void atomic_or_64(volatile uint64_t *, uint64_t);
void atomic_and_64(volatile uint64_t *, uint64_t);
void membar_sync(void);
void membar_enter(void);
void membar_exit(void);
void membar_producer(void);
void membar_consumer(void);

#endif /* HAVE_INLINE_ASM */

/*
 * Aligned 32-bit and 64-bit loads and stores are single-copy atomic on
 * amd64. The volatile access keeps the compiler from tearing, merging,
 * or eliding them.
 */
#define atomic_load_32(p)       (*(volatile const uint32_t *)(p))
#define atomic_load_64(p)       (*(volatile const uint64_t *)(p))
#define atomic_store_32(p, v)   (*(volatile uint32_t *)(p) = (v))
#define atomic_store_64(p, v)   (*(volatile uint64_t *)(p) = (v))

#define atomic_add_32(p, d)     ((void)atomic_add_32_nv((p), (d)))
#define atomic_add_64(p, d)     ((void)atomic_add_64_nv((p), (d)))
#define atomic_inc_32_nv(p)     atomic_add_32_nv((p), 1)
#define atomic_dec_32_nv(p)     atomic_add_32_nv((p), -1)
#define atomic_inc_64_nv(p)     atomic_add_64_nv((p), 1)
#define atomic_dec_64_nv(p)     atomic_add_64_nv((p), -1)

/* Pointers are 64 bits on amd64. */
#define atomic_cas_ptr(p, e, n)                                         \
        ((void *)(uintptr_t)atomic_cas_64((volatile uint64_t *)(p),     \
            (uint64_t)(uintptr_t)(e), (uint64_t)(uintptr_t)(n)))
#define atomic_swap_ptr(p, n)                                           \
        ((void *)(uintptr_t)atomic_swap_64((volatile uint64_t *)(p),    \
            (uint64_t)(uintptr_t)(n)))

#endif /* _MACHINE_ATOMIC_H_ */
