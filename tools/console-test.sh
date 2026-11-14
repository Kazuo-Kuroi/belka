#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Boot under QEMU with the serial console on a Unix socket, and drive it
# with tools/condrive through a script. Passes if the script runs to the
# end and QEMU then exits with 33, a clean halt (the isa-debug-exit
# device). The console's output is saved to the log file and echoed.
#
# usage: console-test.sh condrive script logfile qemu-command...
#

set -u

if [ $# -lt 4 ]; then
        echo "usage: $0 condrive script logfile qemu-command..." >&2
        exit 1
fi
condrive=$1
script=$2
log=$3
shift 3
timeout_secs=${QEMU_TIMEOUT:-120}

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
sock=$dir/console

timeout "$timeout_secs" "$@" \
    -chardev "socket,id=con,path=$sock,server=on,wait=on" \
    -serial chardev:con </dev/null &
qemu=$!
"$condrive" "$sock" "$script" "$log"
drive=$?
# A script that stopped early leaves the guest running: stop it.
if [ "$drive" -ne 0 ]; then
        sleep 2
        kill "$qemu" 2>/dev/null
fi
wait "$qemu"
status=$?
cat "$log"
echo

case $status in
33)     got="clean halt" ;;
35)     got="kernel panic" ;;
124)    got="timed out after ${timeout_secs}s" ;;
*)      got="QEMU exit status $status" ;;
esac
if [ "$drive" -ne 0 ]; then
        echo "test: FAIL (the script stopped; $got)" >&2
        exit 1
fi
if [ "$status" -ne 33 ]; then
        echo "test: FAIL ($got)" >&2
        exit 1
fi
echo "test: PASS (the session ran; $got)"
