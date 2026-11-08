#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Boot the kernel under QEMU and report whether it passed. The kernel
# signals its result through the isa-debug-exit device: QEMU exits with
# status 33 on success and 35 after a panic. Serial output is saved to
# the log file and echoed.
#
# usage: qemu-test.sh logfile qemu-command...
#

set -u

if [ $# -lt 2 ]; then
        echo "usage: $0 logfile qemu-command..." >&2
        exit 1
fi

log=$1
shift
timeout_secs=${QEMU_TIMEOUT:-60}

timeout "$timeout_secs" "$@" -serial "file:$log" </dev/null
status=$?
cat "$log"

case $status in
33)
        echo "test: PASS"
        exit 0
        ;;
35)
        echo "test: FAIL (kernel panic)" >&2
        ;;
124)
        echo "test: FAIL (timed out after ${timeout_secs}s)" >&2
        ;;
*)
        echo "test: FAIL (QEMU exit status $status)" >&2
        ;;
esac
exit 1
