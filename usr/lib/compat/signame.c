/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * sys_signame[]: each signal's name without "SIG", by number, as BSD
 * has it, for the imported programs that print them (xargs). The
 * numbers are Belka's (System V's), from <signal.h>; a number with no
 * signal has no name (NULL).
 */

#include <signal.h>
#include <stddef.h>

#define S(name) [SIG##name] = #name

const char *const sys_signame[NSIG] = {
        [0] = "Signal 0",
        S(HUP), S(INT), S(QUIT), S(ILL), S(TRAP), S(ABRT),
#ifdef SIGEMT
        S(EMT),
#endif
        S(FPE), S(KILL), S(BUS), S(SEGV), S(SYS), S(PIPE), S(ALRM),
        S(TERM), S(USR1), S(USR2), S(CHLD),
#ifdef SIGPWR
        S(PWR),
#endif
        S(WINCH), S(URG), S(IO), S(STOP), S(TSTP), S(CONT), S(TTIN),
        S(TTOU), S(VTALRM), S(PROF), S(XCPU), S(XFSZ),
};
