#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
set -eu
work=${1:-/var/tmp/mlibc-native}
work=$(CDPATH= cd -- "$work" && pwd)
stage=$work/sysroot
cat > "$work/check.c" <<'C'
#define _POSIX_C_SOURCE 200112L
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static __thread int value = 7;
static volatile sig_atomic_t caught;
static void handler(int signal) { caught = signal; }
static void *worker(void *argument)
{
        *(int *)argument = ++value;
        return NULL;
}
int main(void)
{
        pthread_t thread;
        int observed = 0;
        struct sigaction action = { 0 };
        char *memory = calloc(32, 1);
        if (!memory)
                return 1;
        strcpy(memory, "native libc");
        if (strcmp(memory, "native libc"))
                return 2;
        free(memory);
        if (pthread_create(&thread, NULL, worker, &observed) ||
            pthread_join(thread, NULL) || observed != 8 || value != 7)
                return 3;
        action.sa_handler = handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR1, &action, NULL) || raise(SIGUSR1) || caught != SIGUSR1)
                return 4;
        puts("native libc: memory, threads, TLS, signals OK");
        return 0;
}
C
gcc --sysroot="$stage" -static "$work/check.c" -o "$work/check-static"
"$work/check-static"
gcc --sysroot="$stage" -c "$work/check.c" -o "$work/check.o"
ld -64 -dy -z defs -I "$stage/usr/lib64/ld.so.1" -R "$stage/usr/lib64" \
    -o "$work/check-dynamic" "$stage/usr/lib64/crt1.o" \
    "$stage/usr/lib64/crti.o" /usr/lib64/crtbegin.o "$work/check.o" \
    -L"$stage/usr/lib64" -lc /usr/lib64/libcompiler_rt.a \
    /usr/lib64/crtend.o "$stage/usr/lib64/crtn.o"
"$work/check-dynamic"
echo 'native-mlibc: static and shared runtime checks passed'

# Rebuild the project's libc regression suite against the native outputs.
kit=/usr/src/mlibc-build
gcc --sysroot="$stage" -std=c99 -O2 -c "$kit/tests/libctest.c" -o "$work/libctest.o"
gcc --sysroot="$stage" -std=c99 -O2 -c "$kit/tests/hello.c" -o "$work/hello.o"
link_dynamic()
{
        ld -64 -dy -z defs -I "$stage/usr/lib64/ld.so.1" -R "$stage/usr/lib64" \
            -o "$1" "$stage/usr/lib64/crt1.o" "$stage/usr/lib64/crti.o" \
            /usr/lib64/crtbegin.o "$2" -L"$stage/usr/lib64" -lc \
            /usr/lib64/libcompiler_rt.a /usr/lib64/crtend.o "$stage/usr/lib64/crtn.o"
}
for kind in static dynamic; do
        # The kernel suite uses a data mount separate from the root mount.
        root=/tmp/native-libc-$kind-$$
        mkdir -p "$root/bin" "$root/many"
        i=0
        while [ "$i" -lt 200 ]; do
                : > "$root/many/f$i"
                i=$((i + 1))
        done
        for program in libctest hello; do
                if [ "$kind" = static ]; then
                        gcc --sysroot="$stage" -static "$work/$program.o" -o "$root/bin/$program"
                else
                        link_dynamic "$root/bin/$program" "$work/$program.o"
                fi
        done
        (cd "$root"; PATH="$root/bin:$PATH" BELKA=1 libctest "$root")
        rm -rf "$root"
done
echo 'native-mlibc: static and shared libc regression suites passed'
