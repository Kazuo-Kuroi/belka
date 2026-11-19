#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
set -eu
export ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no ac_cv_func_sbrk=no
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$repo/build/toolchain-native-binutils"
${MAKE:-make} -j"${JOBS:-8}" all-binutils MAKEINFO=true \
    AR=ar RANLIB=ranlib AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib
