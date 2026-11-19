#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
# Sourced by utiltest.sh with its check/status helpers and temporary directory.

echo "compilertest: C driver, linking, and TLS"
check "GCC target" x86_64-unknown-belka gcc -dumpmachine
check "GCC version" 14.2.0 gcc -dumpfullversion
cat > compiler-c.c <<'C'
#include <stdio.h>
#include <stdint.h>
#ifndef __belka__
#error Belka target macro is missing
#endif
typedef char pointer_is_64_bits[sizeof(void *) == 8 ? 1 : -1];
int main(void)
{
        volatile uint64_t value = 294;
        printf("C target %lu\n", (unsigned long)(value / 7));
        return 0;
}
C
status "native C99 compile" 0 gcc -std=c99 -Werror -O2 compiler-c.c -o compiler-c
check "native C99 executable" 'C target 42' ./compiler-c
status "native static C compile" 0 gcc -std=c99 -O2 -static compiler-c.c -o compiler-static
check "native static C executable" 'C target 42' ./compiler-static
status "native compiler pipes" 0 gcc -pipe -O2 compiler-c.c -o compiler-pipe
check "native piped compile" 'C target 42' ./compiler-pipe
status "native PIE compile" 0 gcc -fPIE -pie -O2 compiler-c.c -o compiler-pie
check "native PIE executable" 'C target 42' ./compiler-pie
cat > compiler-readonly.c <<'C'
#define _POSIX_C_SOURCE 200112L
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
const char readonly_message[] = "read-only";
void write_readonly(const char *);
long signed_textrel(void);
uint32_t unsigned_textrel(void);
int main(void)
{
        int status;
        pid_t child;
        if (signed_textrel() != -1 || unsigned_textrel() != UINT32_MAX)
                return 1;
        child = fork();
        if (child < 0)
                return 2;
        if (child == 0) {
                write_readonly(readonly_message);
                _exit(99);
        }
        if (waitpid(child, &status, 0) != child)
                return 3;
        return !(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
C
cat > compiler-readonly.s <<'ASM'
.text
.globl write_readonly
.type write_readonly,@function
write_readonly:
        movb $88, (%rdi)
        ret
.weak missing_textrel_symbol
.globl signed_textrel
.type signed_textrel,@function
signed_textrel:
        movq $missing_textrel_symbol-1, %rax
        ret
.globl unsigned_textrel
.type unsigned_textrel,@function
unsigned_textrel:
        movl $missing_textrel_symbol+0xffffffff, %eax
        ret
.section .note.GNU-stack,"",@progbits
ASM
status "native narrow relocations compile" 0 gcc compiler-readonly.c compiler-readonly.s -o compiler-readonly
status "native text protection and narrow relocations" 0 ./compiler-readonly

cat > compiler-dso.c <<'C'
/* Exercise GNU TLS code generation in a shared object. */
static __thread int counter = 40;
int next_value(void);
int next_value(void) { return ++counter; }
C
cat > compiler-dso-main.c <<'C'
#include <stdio.h>
int next_value(void);
int main(void)
{
        int first = next_value();
        int second = next_value();
        printf("TLS %d %d\n", first, second);
        return 0;
}
C
status "native shared object" 0 gcc -fPIC -shared compiler-dso.c -o libcompiler-test.so
status "native DSO client" 0 gcc compiler-dso-main.c -L. -lcompiler-test -Wl,-R,"$PWD" -o compiler-dso
check "native DSO TLS" 'TLS 41 42' ./compiler-dso
echo "compilertest: C++ exceptions and threads"
cat > compiler-cpp.cc <<'CPP'
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <vector>
static int destroyed;
struct guard { ~guard() { ++destroyed; } };
int main()
{
        std::vector<int> values {1, 2, 3};
        try {
                guard cleanup;
                throw std::runtime_error("Belka");
        } catch (const std::runtime_error &) {
                std::printf("C++ %d %d\n", destroyed,
                    std::accumulate(values.begin(), values.end(), 0));
        }
        return destroyed != 1;
}
CPP
status "native C++17 compile" 0 g++ -std=c++17 -O2 compiler-cpp.cc -o compiler-cpp
check "native C++ exceptions" 'C++ 1 6' ./compiler-cpp
status "native static C++ compile" 0 g++ -std=c++17 -static compiler-cpp.cc -o compiler-cpp-static
check "native static C++ exceptions" 'C++ 1 6' ./compiler-cpp-static
cat > compiler-lifetime.cc <<'CPP'
#include <cstdio>
static int initialized;
struct lifetime {
        lifetime() { initialized = 42; }
        ~lifetime() { std::puts("global destructor"); }
};
static lifetime object;
int main()
{
        std::printf("global constructor %d\n", initialized);
        return initialized != 42;
}
CPP
status "native C++ lifetime compile" 0 g++ compiler-lifetime.cc -o compiler-lifetime
check "native C++ global lifetime" 'global constructor 42
global destructor' ./compiler-lifetime
status "native static C++ lifetime compile" 0 g++ -static compiler-lifetime.cc -o compiler-lifetime-static
check "native static C++ global lifetime" 'global constructor 42
global destructor' ./compiler-lifetime-static
cat > compiler-thread.cc <<'CPP'
#include <cstdio>
#include <thread>
static thread_local int value = 7;
int main()
{
        int observed = 0;
        std::thread worker([&] { observed = ++value; });
        worker.join();
        std::printf("thread TLS %d %d\n", observed, value);
        return 0;
}
CPP
status "native C++ thread compile" 0 g++ -std=c++17 -pthread compiler-thread.cc -o compiler-thread
check "native C++ thread TLS" 'thread TLS 8 7' ./compiler-thread
echo "compilertest: multiprecision libraries and diagnostics"
cat > compiler-math.c <<'C'
#include <stdio.h>
#include <gmp.h>
#include <mpfr.h>
#include <mpc.h>
int main(void)
{
        mpz_t integer;
        mpfr_t real;
        mpc_t complex_value;
        mpz_init_set_ui(integer, 42);
        mpfr_init2(real, 128);
        mpc_init2(complex_value, 128);
        mpfr_set_z(real, integer, MPFR_RNDN);
        mpc_set_fr(complex_value, real, MPC_RNDNN);
        printf("multiprecision %lu\n",
            mpfr_get_ui(mpc_realref(complex_value), MPFR_RNDN));
        mpc_clear(complex_value);
        mpfr_clear(real);
        mpz_clear(integer);
        return 0;
}
C
status "native multiprecision compile" 0 gcc compiler-math.c -lmpc -lmpfr -lgmp -lm -o compiler-math
check "native GMP MPFR MPC" 'multiprecision 42' ./compiler-math
printf 'int main(void) { return missing_identifier; }\n' > compiler-invalid.c
status "native compiler error" 1 gcc -fsyntax-only compiler-invalid.c
printf 'extern int missing_function(void); int main(void) { return missing_function(); }\n' > compiler-missing.c
status "native linker error" 1 gcc compiler-missing.c -o compiler-missing
