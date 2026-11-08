/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Compiler abstraction. Every use of a compiler extension in the kernel
 * goes through a macro defined here, and every macro has a fallback for
 * a plain C99 compiler. A fallback either expands to nothing (the
 * extension is only a hint) or calls an out-of-line routine with the
 * same effect.
 *
 * HAVE_INLINE_ASM is defined when GNU-style inline assembly may be used.
 * Machine-dependent headers test it to choose between inline and
 * out-of-line versions of their primitives. Defining BELKA_NO_INLINE_ASM
 * forces the out-of-line versions, which is how the fallback paths are
 * tested.
 */

#ifndef _SYS_COMPILER_H_
#define _SYS_COMPILER_H_

#if defined(__GNUC__) || defined(__clang__)
#define BELKA_GNUC 1
#endif

#if defined(BELKA_GNUC) && !defined(BELKA_NO_INLINE_ASM)
#define HAVE_INLINE_ASM 1
#endif

#ifdef BELKA_GNUC
#define __dead                  __attribute__((__noreturn__))
#define __unused                __attribute__((__unused__))
#define __printflike(fmt, arg)  \
        __attribute__((__format__(__printf__, fmt, arg)))
#define __predict_true(e)       __builtin_expect(((e) != 0), 1)
#define __predict_false(e)      __builtin_expect(((e) != 0), 0)
#else
#define __dead
#define __unused
#define __printflike(fmt, arg)
#define __predict_true(e)       ((e) != 0)
#define __predict_false(e)      ((e) != 0)
#endif

/*
 * __insn_barrier() stops the compiler from moving memory accesses across
 * it. It is not a hardware memory barrier; see <machine/atomic.h> for
 * those. Without inline assembly a call to an external function serves,
 * because the compiler must assume the callee reads and writes memory.
 */
#ifdef HAVE_INLINE_ASM
#define __insn_barrier()        __asm__ __volatile__("" ::: "memory")
#else
void insn_barrier_func(void);
#define __insn_barrier()        insn_barrier_func()
#endif

/*
 * CTASSERT(expr) fails to compile when the constant expression is false.
 * It may be used at file scope or block scope.
 */
#define __CONCAT1(a, b)         a ## b
#define __CONCAT(a, b)          __CONCAT1(a, b)
#define CTASSERT(expr)          \
        typedef char __CONCAT(ctassert_, __LINE__)[(expr) ? 1 : -1] __unused

#endif /* _SYS_COMPILER_H_ */
