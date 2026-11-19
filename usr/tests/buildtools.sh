#!/bin/sh
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-19
# SPDX-License-Identifier: MIT
# Sourced by utiltest.sh in its temporary directory.
status "muon version" 0 muon version
status "samurai version" 0 samu --version
mkdir ninja-fixture
cat > ninja-fixture/build.ninja <<'NINJA'
rule capture
  command = printf '$out\n' > $out
build one: capture
build two: capture
rule foreground
  command = printf console > $out
  pool = console
build foreground: foreground
build all: phony one two foreground
default all
NINJA
status "samurai parallel build" 0 samu -C ninja-fixture -j2
check "samurai first output" one cat ninja-fixture/one
check "samurai second output" two cat ninja-fixture/two
check "samurai console job" console cat ninja-fixture/foreground
status "samurai incremental build" 0 samu -C ninja-fixture
status "samurai closed stdout" 0 sh -c 'rm ninja-fixture/one; samu -C ninja-fixture 1>&-'
check "samurai output after closed stdout" one cat ninja-fixture/one
cat > ninja-fixture/fail.ninja <<'NINJA'
rule failure
  command = exit 7
build failed: failure
NINJA
status "samurai failure status" 1 samu -C ninja-fixture -f fail.ninja
if command -v gcc >/dev/null 2>&1; then
        mkdir muon-fixture
        cat > muon-fixture/meson.build <<'MESON'
project('belka-build-tools', 'c')
assert(host_machine.system() == 'belka')
base = static_library('base', 'base.c')
lib = shared_library('answer', 'answer.c', link_with: base, soversion: '1')
exe = executable('answer-test', 'main.c', link_with: lib, install: true)
test('shared answer', exe)
MESON
        echo 'int base_value(void) { return 42; }' > muon-fixture/base.c
        echo 'int base_value(void); int answer(void) { return base_value(); }' > muon-fixture/answer.c
        echo 'int answer(void); int main(void) { return answer() != 42; }' > muon-fixture/main.c
        status "muon native configure" 0 muon -C muon-fixture setup -Dprefix=/usr "$PWD/muon-out"
        status "samurai generated build" 0 samu -C muon-out -j2
        status "muon shared-library test" 0 muon -C muon-out test -j2
        status "muon staged install" 0 muon -C muon-out install -d "$PWD/muon-dest"
        status "muon installed program" 0 test -f muon-dest/usr/bin/answer-test
fi
