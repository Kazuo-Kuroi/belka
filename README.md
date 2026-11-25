# Belka

Belka is a System V-style kernel, starting on x86-64. It draws on
UnixWare for its interfaces and organisation and on IRIX for its
multiprocessing, scheduling, and VFS design. It is MIT-licensed.

Design decisions are recorded in
[doc/design/decisions.md](doc/design/decisions.md). Where files go is
[doc/design/hierarchy.md](doc/design/hierarchy.md). Code follows
[CODING_STYLE.md](CODING_STYLE.md) with 8-space indentation.

## Building

Requirements: GNU make, GCC or Clang, GNU binutils, GNU m4, `xorriso`,
`curl`, QEMU, and OVMF; for the C library, muon (0.7 or later) and
samurai. See [doc/qemu-setup.md](doc/qemu-setup.md), the guide to running
Belka under QEMU. Run `git submodule update --init` first: the
link-editor, the C library and the utilities are submodules (the
utilities' repository is reached over ssh).

```sh
make                    # build build/amd64-gcc/belka
make TOOLCHAIN=clang    # build with Clang
make LINKER=gnu         # link with GNU ld instead of the illumos ld
make run                # boot the root image to a login prompt (root, no
                        # password), serial console on stdio
make run-selftest       # boot the self-tests with the console on stdio
make test               # boot, run the self-tests, and report PASS/FAIL
make test-fault         # boot, force a page fault, expect a clean panic
make test-apfault       # the same, on a secondary processor
make test-stack         # overflow a kernel stack, expect a double fault
make test-uaf           # write to freed kmem, expect it to be caught
make test-reboot        # reboot through the ACPI reset register
make test-boot          # boot the test root image: init, rc, getty, login
make test-console       # type a session at the serial console, as a person
make test-all           # every test above
```

The system programs are built statically against mlibc and put on an
ext2 root image by `tools/mkroot.sh` (it needs `fakeroot`): NetBSD's
`init`, `login` and `su` (`contrib/netbsd`), mksh as `/usr/bin/sh`
(`contrib/mksh`), and Belka's own `getty`, `reboot`/`halt`, System
V's `mount`/`umount` and `rcorder` (`usr/sbin`), over a small compat
library (`usr/lib/compat`), and in `/usr/bin` the Ksenon utilities, `ls`,
`cat`, `cp` and the rest (`contrib/ksenon-utils`, mostly linked
dynamically), Belka's `ps` over IRIX-style `/proc` and its `passwd`,
NetBSD's terminfo (`tic`, `tput`, `infocmp`; compiled at boot) and
`ed`, and in `/usr/bin` NetBSD's `sed`, `grep`, `awk`, `find`, `xargs`,
`stat` and others, and nvi as `vi`, `ex` and `view` over NetBSD's
curses. `/etc` comes from `etc/`:
`/etc/rc` starts the scripts in `/etc/rc.d` (NetBSD's `rc.subr`) in
`rcorder`'s order, and `/etc/vfstab` says what is mounted (`/tmp` and
`/var/run` are tmpfs); the passwords are in `/etc/shadow`. The image is
laid out as [doc/design/hierarchy.md](doc/design/hierarchy.md) says:
`/bin` is a link to `/usr/bin`, `/sbin` has static copies of what starts
and recovers the system, and homes are in `/usr/people`, root's `/adm`.
`make sys-progs` builds the programs and `make root-image` the image.
A normal boot (no `selftest` on the kernel command line) mounts that
image as root and starts `/sbin/init`.

Every self-test boot attaches `build/test-disk.img`, an 8 MB disk with
a known pattern in each sector, as a virtio block device (`vd0`); it is
built by `tools/mkdisk.c` with the host compiler (`HOSTCC`, default
`cc`). Copies of it go on an NVMe controller (`nvd0`,
`sys/dev/pci/nvme.c`) and on q35's AHCI controller (`sd0`,
`sys/dev/pci/ahci.c`, beside the CD-ROM it passes over), and the
block-device self-tests run on all three. Both drivers time out a
command after 30 seconds and recover the device, which the self-tests
check by losing a completion on purpose. `sd0` uses native command
queueing, 32 commands deep. A second NVMe controller (`nvd1`) has 4 KB
sectors and a copy of the 4 KB-block ext2 image, which the ext2
self-tests use as they do `vd2`. One more copy of the test disk,
`vd3`, has a GUID partition table written into it by `tools/mkgpt.c`; its
partitions (`vd3p1`, `vd3p2`, `vd3p4`: `sys/kern/subr_gpt.c`) are
checked against the pattern, and against the backup table when the
primary is broken. Only GPT is read, not MBR partitions or disklabels.
Two ext2 images are attached as `vd1` (1 KB blocks) and `vd2` (4 KB
blocks). `tools/mkext2.sh` rebuilds them with `mke2fs` before every
boot, because the tests write to them. After `make test`, the host
checks both with `e2fsck -fn`, and `tools/check-ext2.sh` reads back
with `debugfs` the files the kernel left. This needs e2fsprogs
(`E2FSCK` and `DEBUGFS` name the programs).

User-mode test programs live in `usr/tests`. All are built against
mlibc, the C library, with `usr/tests/rawsys.h` for the kernel's own
interfaces that they test directly (LWPs, contexts, the break). mlibc
comes from the `contrib/mlibc` submodule (`git submodule update
--init`); building it needs muon and samurai, C implementations of meson
and ninja (`MUON` and `SAMU` name them), and its first configure
fetches mlibc's own subprojects with git. mlibc is built with the
kernel's compiler (GCC 14 or later, or Clang), as `libc.a` and as the
runtime linker `ld.so.1` and `libc.so.1`, and installed into
`build/sysroot-$(TOOLCHAIN)/usr/lib64`; `make mlibc` builds it alone.
Programs are linked against `libc.so.1` and name `/usr/lib64/ld.so.1`
as their interpreter (`libctest-static` is linked statically,
`dyntest-pie` is position-independent). All the programs are installed
in `/bin` on both test images, the shared C library in `/usr/lib64` and
the tests' own `libdt.so.1` in `/lib`, and the kernel runs them during
`make test`. `make user-progs` builds them alone. The STREAMS framework
(`sys/kern/str_*.c` and the stream head, `strhead.c`), on which the
network stack is being built, is tested by the kernel's self-tests and
by `strtest`, through the loopback driver `/dev/strloop`, the test
modules `strpass` and `strup`, and the multiplexing driver `/dev/strmux`.
The self-test boot also has a virtio-net device (`/dev/vionet0`, a
DLPI provider) on QEMU's user-mode network; `nettest` talks ARP with
QEMU's gateway through it. `iptest` links it under `/dev/ip` (IPv4 and
ARP, `sys/netinet`), configures it, and pings the gateway, itself and
127.0.0.1. `udptest` links an IP stream under `/dev/udp` (a TPI
provider) and uses `AF_INET` datagram sockets over the loopback, and
`tcptest` does the same with `/dev/tcp` and stream sockets; both check
the socket options, and `tcptest` non-blocking connects and `poll`. The
self-test boot's two more virtio-net devices share a QEMU hub, on which
`fwdtest` forwards through IP and plays the other hosts, and checks
reassembly and the ICMP errors that reach the sockets. Intel's 82574
(QEMU's `e1000e`, by MSI-X) and 82540EM (QEMU's `e1000`, by INTx, which
the ACPI `_PRT` routes through uACPI, `sys/external/uacpi`) are on that
hub too, driven by `sys/dev/pci/e1000g.c`: `e1000test` sends frames of
every size both ways between each and a virtio-net device, a broadcast
and bursts larger than its rings, and UDP through IP over it. Both
drivers share the Ethernet DLPI provider, `sys/net/dlether.c`. On a normal
boot, `/etc/rc.d/network` plumbs the stack with `netcfg(8)` and
attaches `vionet0`, `/etc/rc.d/dhcpcd` leases its address from QEMU's
DHCP server with NetBSD's dhcpcd (`contrib/dhcpcd`), as `/etc/rc.conf`
says, and `/etc/rc.d/staticroute` adds the routes in
`/etc/routed/static_route.conf`, which `boottest` checks;
`/etc/rc.d/dnscache` starts djbdns' dnscache (`contrib/djbdns`),
chrooted, as the resolver `/etc/resolv.conf` names (resolving from the
root servers, or forwarding to the ones the leases offer), which
`boottest` checks through fake servers of its own.
The network tools are NetBSD's ping and whois, Belka's traceroute, djbdns'
client tools with Belka's `host` and `dig` (and `drill`) over them, and
`xdig`, which reports all DNS and WHOIS say of a domain; ping and
traceroute use raw sockets (`SOCK_RAW`), the superuser's, and are
set-user-ID root.
The SSH programs are Dropbear's (`contrib/dropbear`), installed under
OpenSSH's names (`sshd`, `ssh`, `ssh-keygen`, `scp`), with `sshd` run for
each connection by `/etc/rc.d/inetd`: ucspi-tcp's `tcpserver`
(`contrib/ucspi-tcp`) for each service of `/etc/inetd.conf`, with
tcprules' access rules. `boottest` logs in over the loopback by key, by
password and on a pseudo-terminal, and copies with `scp`;
`make run SSH_PORT=2222` forwards a host port to it.
Everything is linked by the illumos link-editor, from the
`contrib/illumos-ld` submodule, which the build compiles for the host
first (`build/tools/illumos-ld/ld`); `LINKER=gnu` uses the host's GNU
ld instead, with its outputs kept apart. The shared objects are linked
by the illumos link-editor either way, through `tools/ld/illumos-cc`.
Settings for one host, such as `GCC_VERSION=14` for `gcc-14` and
`g++-14` or `TOOLCHAIN=clang`, may go in an untracked `config.mk`.
`SMP=n` and `MEM=size` set the guest's processors and memory, and
`QEMU_CPU` its processor model (default `max,+invtsc`: Belka's
nanosecond clock needs an invariant TSC).
`make INLINE_ASM=no` builds the out-of-line fallbacks for every inline
assembly primitive, which is what a compiler without GNU extensions
would use. `WERROR=yes` turns warnings into errors. The first ISO build
downloads a pinned, checksummed Limine release into `build/`.

## Source tree

| Path | Contents |
|---|---|
| `sys/kern` | machine-independent kernel |
| `sys/vm` | physical pages, the page cache, address spaces, anonymous memory |
| `usr/tests` | user-mode test programs and their tiny C library |
| `contrib/mlibc` | the C library (a submodule) |
| `contrib/illumos-ld` | the illumos link-editor (a submodule, CDDL) |
| `usr/lib/crt` | start and end objects for programs the illumos link-editor links |
| `tools/ld` | link maps for user programs |
| `tools/mlibc`, `tools/abicheck` | mlibc's cross files, and its ABI check against the kernel |
| `sys/libkern` | freestanding C library routines |
| `sys/sys` | machine-independent headers (`<sys/...>`) |
| `sys/dev/ic` | drivers for chips, independent of the bus |
| `sys/dev/acpi`, `sys/dev/pci` | ACPI tables, PCI bus support |
| `sys/dev/virtio` | virtio transport, virtqueues, and drivers |
| `sys/fs` | file systems: tmpfs and ext2 |
| `sys/netinet` | IPv4, ICMP and ARP, as the `/dev/ip` STREAMS driver, and UDP and TCP as `/dev/udp` and `/dev/tcp` |
| `sys/arch/amd64/amd64` | amd64 machine-dependent code |
| `sys/arch/amd64/include/machine` | amd64 headers (`<machine/...>`) |
| `sys/arch/amd64/conf` | source lists and linker script |
| `sys/external` | imported third-party code, under its own licence |
| `mk` | toolchain definitions |
| `tools` | build and test scripts |
