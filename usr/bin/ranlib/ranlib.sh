#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-17
# SPDX-License-Identifier: MIT
# illumos ar -s writes an archive's symbol index without changing members.
if [ "$#" -eq 0 ]; then
        echo 'usage: ranlib archive ...' >&2
        exit 1
fi
result=0
for archive do
        /usr/bin/ar -s "$archive" || result=1
done
exit "$result"
