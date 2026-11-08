# Belka

Belka is a System V-style kernel, starting on x86-64. It draws on
UnixWare for its interfaces and organisation and on IRIX for its
multiprocessing, scheduling, and VFS design. It is MIT-licensed.

Design decisions are recorded in
[doc/design/decisions.md](doc/design/decisions.md). Code follows
[CODING_STYLE.md](CODING_STYLE.md) with 8-space indentation.

## Building

Requirements: GNU make, GCC or Clang (with `ld.lld`), `xorriso`, `curl`,
QEMU, and OVMF. See [doc/qemu-setup.md](doc/qemu-setup.md).

```sh
make                    # build build/amd64-gcc/belka
make TOOLCHAIN=clang    # build with Clang and ld.lld
make run                # boot in QEMU with the serial console on stdio
make test               # boot, run the self-tests, and report PASS/FAIL
make test-fault         # boot, force a page fault, expect a clean panic
make test-apfault       # the same, on a secondary processor
make test-stack         # overflow a kernel stack, expect a double fault
make test-uaf           # write to freed kmem, expect it to be caught
make test-all           # every test above
```

`SMP=n` and `MEM=size` set the guest's processors and memory.
`make INLINE_ASM=no` builds the out-of-line fallbacks for every inline
assembly primitive, which is what a compiler without GNU extensions
would use. `WERROR=yes` turns warnings into errors. The first ISO build
downloads a pinned, checksummed Limine release into `build/`.

## Source tree

| Path | Contents |
|---|---|
| `sys/kern` | machine-independent kernel |
| `sys/libkern` | freestanding C library routines |
| `sys/sys` | machine-independent headers (`<sys/...>`) |
| `sys/dev/ic` | drivers for chips, independent of the bus |
| `sys/arch/amd64/amd64` | amd64 machine-dependent code |
| `sys/arch/amd64/include/machine` | amd64 headers (`<machine/...>`) |
| `sys/arch/amd64/conf` | source lists and linker script |
| `sys/external` | imported third-party code, under its own licence |
| `mk` | toolchain definitions |
| `tools` | build and test scripts |
