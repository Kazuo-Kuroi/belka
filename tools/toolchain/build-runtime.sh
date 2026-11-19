#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$repo/build/toolchain-cross-gcc"
# Install start files and libgcc before configuring the C++ runtime's links.
${MAKE:-make} -j"${JOBS:-8}" all-target-libgcc \
    AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib NM_FOR_TARGET=nm STRIP_FOR_TARGET=strip
${MAKE:-make} install-target-libgcc \
    AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib NM_FOR_TARGET=nm STRIP_FOR_TARGET=strip
${MAKE:-make} -j"${JOBS:-8}" all-target-libstdc++-v3 \
    AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib NM_FOR_TARGET=nm STRIP_FOR_TARGET=strip \
    MAKEINFO=true
${MAKE:-make} install-target-libstdc++-v3 \
    AR_FOR_TARGET=ar RANLIB_FOR_TARGET=ranlib NM_FOR_TARGET=nm STRIP_FOR_TARGET=strip \
    MAKEINFO=true
