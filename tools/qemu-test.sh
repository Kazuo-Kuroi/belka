#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Boot the kernel under QEMU and check the outcome. The kernel signals
# its result through the isa-debug-exit device: QEMU exits with status
# 33 after a clean halt and 35 after a panic. Serial output is saved to
# the log file and echoed. If QEMU_EXPECT is set, the log must also
# contain that text (each of its lines, if it has several). Whatever the
# expected outcome, a self-test failure line in the log fails the run.
#
# With "reset", the kernel is expected to reset the machine, which QEMU
# run with -no-reboot turns into exit status 0.
#
# usage: qemu-test.sh pass|panic|reset logfile qemu-command...
#

set -u

if [ $# -lt 3 ]; then
        echo "usage: $0 pass|panic|reset logfile qemu-command..." >&2
        exit 1
fi

case $1 in
pass)   want=33 ;;
panic)  want=35 ;;
reset)  want=0 ;;
*)
        echo "$0: expected outcome must be pass, panic, or reset" >&2
        exit 1
        ;;
esac
log=$2
shift 2
timeout_secs=${QEMU_TIMEOUT:-60}

timeout "$timeout_secs" "$@" -serial "file:$log" </dev/null
status=$?
cat "$log"

case $status in
0)      got="machine reset" ;;
33)     got="clean halt" ;;
35)     got="kernel panic" ;;
124)    got="timed out after ${timeout_secs}s" ;;
*)      got="QEMU exit status $status" ;;
esac

if grep -qE '^selftest: [^ ]+:[0-9]+: ' "$log"; then
        echo "test: FAIL ($got, with self-test failures)" >&2
        exit 1
fi
if [ "$status" -eq "$want" ]; then
        # Each line of QEMU_EXPECT must be in the log.
        nl='
'
        oldifs=$IFS
        IFS=$nl
        for text in ${QEMU_EXPECT:-}; do
                if ! grep -qF -- "$text" "$log"; then
                        echo "test: FAIL ($got, but \"$text\" not seen)" >&2
                        exit 1
                fi
        done
        IFS=$oldifs
        echo "test: PASS ($got)"
        exit 0
fi
echo "test: FAIL ($got)" >&2
exit 1
