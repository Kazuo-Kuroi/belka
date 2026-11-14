/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Run by utest with argv { "argtest", "one", "two words" } and envp
 * { "FOO=bar", "X=1" }: check what exec built. Exits 7 if all is well,
 * or 100 plus the number of failed checks.
 */

#define _DEFAULT_SOURCE         /* getauxval */

#include <elf.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

extern char _start[];

int
main(int argc, char **argv, char **envp)
{
        unsigned long pagesz = getauxval(AT_PAGESZ);
        unsigned long entry = getauxval(AT_ENTRY);
        unsigned long phdr = getauxval(AT_PHDR);
        unsigned long phnum = getauxval(AT_PHNUM);
        int bad = 0;

        if (argc != 3 || strcmp(argv[0], "argtest") != 0 ||
            strcmp(argv[1], "one") != 0 || strcmp(argv[2], "two words") != 0 ||
            argv[3] != NULL)
                bad++;
        if (envp[0] == NULL || strcmp(envp[0], "FOO=bar") != 0 ||
            envp[1] == NULL || strcmp(envp[1], "X=1") != 0 || envp[2] != NULL)
                bad++;
        if (pagesz != 4096 || entry != (unsigned long)_start)
                bad++;
        /* The program headers are ours, mapped where AT_PHDR says. */
        if (phdr == 0 || phnum == 0 ||
            ((const Elf64_Phdr *)phdr)[0].p_type == PT_NULL)
                bad++;
        /* The stack is 16-byte aligned at entry: argv is 8 above it. */
        if (((unsigned long)argv & 15) != 8)
                bad++;
        if (bad != 0)
                printf("argtest: %d checks failed\n", bad);
        return bad == 0 ? 7 : 100 + bad;
}
