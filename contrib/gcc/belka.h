/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-18
 * SPDX-License-Identifier: MIT
 *
 * GCC target policy for Belka's amd64 System V ABI and illumos linker.
 */

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS() do { \
        builtin_define("__belka__"); \
        builtin_define("__unix__"); \
        builtin_assert("system=belka"); \
        builtin_assert("system=unix"); \
        builtin_assert("system=posix"); \
} while (0)

#undef CPP_SPEC
#define CPP_SPEC "%{pthread:-D_REENTRANT}"

#undef STARTFILE_SPEC
#define STARTFILE_SPEC \
        "%{!shared:crt1.o%s} crti.o%s %{!shared:belka-crtbegin.o%s} " \
        "%{shared|pie:crtbeginS.o%s;:crtbegin.o%s}"
#undef ENDFILE_SPEC
#define ENDFILE_SPEC \
        "%{shared|pie:crtendS.o%s;:crtend.o%s} %{!shared:belka-crtend.o%s} crtn.o%s"
#undef LIB_SPEC
#define LIB_SPEC "-lc"
#undef LIBGCC_SPEC
#define LIBGCC_SPEC "-lgcc"
#undef LINK_SPEC
#define LINK_SPEC \
        "-64 %{shared:-G -dy} %{static:-dn} " \
        "%{!shared:%{!static:-dy -I /usr/lib64/ld.so.1}} " \
        "%{pie:-z type=pie} -z defs"
/* LINK_SPEC already translates PIE to the illumos option. */
#undef LINK_PIE_SPEC
#define LINK_PIE_SPEC ""
#undef LINK_EH_SPEC
#define LINK_EH_SPEC ""

#undef STANDARD_STARTFILE_PREFIX_1
#define STANDARD_STARTFILE_PREFIX_1 "/usr/lib64/"
#undef STANDARD_STARTFILE_PREFIX_2
#define STANDARD_STARTFILE_PREFIX_2 ""

#define TARGET_POSIX_IO

#undef SYSROOT_SPEC
#define SYSROOT_SPEC ""

/* Keep unwind-table flags compatible with mlibc and the illumos linker. */
#undef EH_TABLES_CAN_BE_READ_ONLY
#define EH_TABLES_CAN_BE_READ_ONLY 1
