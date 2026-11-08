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
make test-reboot        # reboot through the ACPI reset register
make test-all           # every test above
```

Every QEMU run attaches `build/test-disk.img`, an 8 MB disk with a known
pattern in each sector, as a virtio block device (`vd0`); it is built by
`tools/mkdisk.c` with the host compiler (`HOSTCC`, default `cc`).
Two ext2 images are attached as `vd1` (1 KB blocks) and `vd2` (4 KB
blocks). `tools/mkext2.sh` rebuilds them with `mke2fs` before every
boot, because the tests write to them. After `make test`, the host
checks both with `e2fsck -fn`, and `tools/check-ext2.sh` reads back
with `debugfs` the files the kernel left. This needs e2fsprogs
(`E2FSCK` and `DEBUGFS` name the programs).
`SMP=n` and `MEM=size` set the guest's processors and memory.
`make INLINE_ASM=no` builds the out-of-line fallbacks for every inline
assembly primitive, which is what a compiler without GNU extensions
would use. `WERROR=yes` turns warnings into errors. The first ISO build
downloads a pinned, checksummed Limine release into `build/`.

## Source tree

| Path | Contents |
|---|---|
| `sys/kern` | machine-independent kernel |
| `sys/vm` | physical pages, kernel virtual space, and the page cache |
| `sys/libkern` | freestanding C library routines |
| `sys/sys` | machine-independent headers (`<sys/...>`) |
| `sys/dev/ic` | drivers for chips, independent of the bus |
| `sys/dev/acpi`, `sys/dev/pci` | ACPI tables, PCI bus support |
| `sys/dev/virtio` | virtio transport, virtqueues, and drivers |
| `sys/fs` | file systems: tmpfs and ext2 |
| `sys/arch/amd64/amd64` | amd64 machine-dependent code |
| `sys/arch/amd64/include/machine` | amd64 headers (`<machine/...>`) |
| `sys/arch/amd64/conf` | source lists and linker script |
| `sys/external` | imported third-party code, under its own licence |
| `mk` | toolchain definitions |
| `tools` | build and test scripts |
