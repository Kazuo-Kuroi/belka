# Running Belka under QEMU

Belka runs on QEMU's `q35` machine with UEFI firmware (OVMF). The kernel
boots from an ISO image made with the Limine boot loader. Its root file
system is an ext2 image on a virtio block device, and its network is a
virtio-net device on QEMU's user-mode network. The console is the first
serial port, which `make run` connects to your terminal.

This guide covers preparing a host, building, booting to a login prompt,
using the system, and running the tests. The examples assume Debian 13;
other Linux distributions need the same pieces under their own package
names.

## 1. Prepare the host

### Packages

```sh
sudo apt install build-essential clang git curl m4 \
    qemu-system-x86 ovmf xorriso mtools fakeroot e2fsprogs
```

- `qemu-system-x86`: `qemu-system-x86_64`.
- `ovmf`: UEFI firmware for QEMU, at `/usr/share/OVMF/OVMF_CODE_4M.fd`.
- `xorriso` and `mtools`: to make the bootable ISO and its FAT image.
- `m4`: GNU m4, which the illumos link-editor's libelf build uses.
- `curl`: the build fetches the Limine boot loader with it.
- `fakeroot`: `tools/mkroot.sh` makes the root image as root through it.
- `e2fsprogs`: `mke2fs`, `e2fsck` and `debugfs`, to make, repair and
  check the ext2 images.

GCC 14 or later, or Clang, builds everything. GCC is the default.

### muon and samurai

The C library (mlibc) is built with muon and samurai, C implementations
of meson and ninja. They must be on `PATH` as `muon` and `samu`, or be
named by `MUON=` and `SAMU=` on the make command line or in `config.mk`.
Debian's `muon-meson` package (0.4) is too old, since it ignores cross
files. Build muon 0.7 or later from <https://muon.build>, and samurai
from <https://github.com/michaelforney/samurai>.

### KVM

QEMU runs much faster with KVM. The build user must be able to open
`/dev/kvm`:

```sh
sudo usermod -aG kvm "$USER"
```

Log in again afterwards; `id` should then list `kvm`. Until then,
`sg kvm -c "make run"` runs one command in the group. If the host is
itself a virtual machine, its hypervisor must allow nested
virtualisation, and the VM's processor type must be `host`.

Without KVM, QEMU falls back to TCG emulation (`accel=kvm:tcg`). That
works, but it is several times slower, and the tests may need longer
timeouts (`QEMU_TIMEOUT=`, in seconds).

### Firmware in another place

If OVMF is somewhere else, say where in `config.mk` at the top of the
tree. That file is local and not kept in git. On Proxmox, for example:

```make
OVMF_CODE := /usr/share/pve-edk2-firmware/OVMF_CODE_4M.fd
```

## 2. Get the sources

```sh
git clone https://github.com/Kazuo-Kuroi/belka.git
cd belka
git submodule update --init
```

The illumos link-editor (`contrib/illumos-ld`), the C library
(`contrib/mlibc`) and the Ksenon utilities (`contrib/ksenon-utils`) are
submodules. The utilities' repository is reached over ssh, so you need
an ssh key that GitHub accepts. The first build also fetches Limine and
mlibc's own subprojects, so it needs the network.

## 3. Build and boot

```sh
make run
```

That builds the kernel, the C library, the system programs and the root
image, then boots it. The first build takes a while; later ones rebuild
only what changed. The pieces can also be built alone:

```sh
make                    # the kernel: build/amd64-gcc/belka
make iso                # the boot ISO: build/amd64-gcc/belka.iso
make root-image         # the root image: build/root-gcc.img
```

At boot, the serial console shows the firmware, Limine, the kernel's
messages and `/etc/rc` starting the system. It ends at a login prompt:

```
login: root
```

`root` has no password. The shell is mksh (`/bin/sh`). An ordinary user
can be added with `useradd` and given a password with `passwd`.

### Leaving

- **`halt`** stops the system cleanly, and QEMU exits. **`reboot`**
  also ends the run, since QEMU is started with `-no-reboot`.
- **Ctrl-A X** quits QEMU at once, as if the power were cut.
- **Ctrl-A C** switches between the console and the QEMU monitor. In the
  monitor, `info kvm` says whether KVM is in use.

Quitting without `halt` leaves the root file system dirty. The kernel
then mounts it read-only, since Belka has no `fsck` yet. `make run`
repairs the image with the host's `e2fsck` before each boot, so the
next `make run` is fine.

### What is kept between runs

The root image, `build/root-gcc.img`, is kept between runs, so files you
make in Belka are still there next time. It is made again, and what you
changed is lost, whenever something that goes into it changes: a
program, a file under `etc/`, or `tools/mkroot.sh`. Keep anything you
want to hold on to outside the image.

## 4. Inside Belka

### The network

QEMU's user-mode network puts Belka behind a NAT, with no host
privileges needed:

| Address    | What it is                          |
|------------|-------------------------------------|
| 10.0.2.15  | Belka (`vionet0`)                   |
| 10.0.2.2   | the gateway, which is also the host |
| 10.0.2.3   | QEMU's DNS server                   |

At boot, `/etc/rc.d/network` sets up the IP stack and attaches
`vionet0`. Then `/etc/rc.d/dhcpcd` leases its address from QEMU's DHCP
server with dhcpcd. To see the result:

```sh
netcfg show             # interfaces, routes and the ARP cache
dhcpcd -U vionet0       # the lease
```

To use a static address instead, set these in `/etc/rc.conf`:

```sh
ifaddr_vionet0="10.0.2.15/24"
defaultroute="10.0.2.2"
```

### Names

The resolver, djbdns' dnscache, listens on 127.0.0.1, and
`/etc/resolv.conf` names it. By default dnscache resolves names itself,
starting from the root servers, which needs outgoing DNS (UDP port 53)
from the host to the Internet. If the host's network blocks that, have
dnscache forward to QEMU's DNS server instead, in `/etc/rc.conf`:

```sh
dnscache_forwarders="DHCP"      # the servers the lease offers (10.0.2.3)
```

Then start dnscache again and try a name:

```sh
sh /etc/rc.d/dnscache restart
dnsip example.com
```

`dnscache_forwarders` can also list addresses of its own. Belka's
kernel does not run `#!` scripts, so give an rc script to `sh` when you
run it by hand.

### What is not there yet

There is no `ping`. There are no network servers, such as telnet or
ssh, so nothing on the host can connect into Belka. The serial console
is the way in.

## 5. Settings

These go on the make command line (`make run SMP=2`) or in `config.mk`:

| Setting           | Default               | What it does                      |
|-------------------|-----------------------|-----------------------------------|
| `SMP`             | `4`                   | the guest's processors            |
| `MEM`             | `512M`                | the guest's memory (128M at least) |
| `QEMU_CPU`        | `max,+invtsc`         | the guest's processor model       |
| `TOOLCHAIN`       | `gcc`                 | `clang` builds with Clang         |
| `LINKER`          | `illumos`             | `gnu` links with the host's GNU ld |
| `OVMF_CODE`       | `/usr/share/OVMF/OVMF_CODE_4M.fd` | the UEFI firmware     |
| `QEMU`            | `qemu-system-x86_64`  | the QEMU to run                   |

Each toolchain and linker has its own build directory and images, for
example `build/amd64-clang` and `build/root-clang.img`, so they can sit
side by side.

Belka's nanosecond clock needs an invariant TSC, which QEMU leaves out
of `max`; hence `+invtsc`. `QEMU_CPU=qemu64` runs Belka on the
clock-tick fallback instead. Below 128 MB, OVMF and Limine run out of
memory before the kernel is loaded.

## 6. The tests

```sh
make test               # the kernel's and user-mode self-tests
make run-selftest       # the same boot, with the console on your terminal
make test-boot          # a full boot: init, rc, networking, login, utilities
make test-console       # a console session typed as a person would
make -j1 test-all       # all of them, and the fault tests
```

Each test boots QEMU without a display, saves the serial output, and
reports `PASS` or `FAIL`. The kernel stops QEMU through its debug-exit
device: status 33 is a clean halt and 35 a panic. The logs are under the
build directory, for example `build/amd64-gcc/test-serial.log` and
`build/amd64-gcc/test-boot-serial.log`.

Run only one test at a time in a checkout. The tests make their disk
images afresh in shared places, so `make -j` across QEMU targets, or two
at once, would trip over each other. `make test` takes about a minute
and a half with KVM; half a minute of that is a deliberate wait for an
IP reassembly timeout. Its timeout is 180 seconds.

## 7. QEMU by hand

`make run` amounts to this. Run it from the top of the tree after a
`make run` or `make iso root-image`, with your own firmware path if it
differs:

```sh
qemu-system-x86_64 \
    -machine q35,accel=kvm:tcg -cpu max,+invtsc -smp 4 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -drive file=build/root-gcc.img,if=none,id=vd0,format=raw \
    -device virtio-blk-pci,drive=vd0,disable-legacy=on \
    -nic none -netdev user,id=n0 \
    -device virtio-net-pci,netdev=n0,disable-legacy=on \
    -display none -no-reboot \
    -cdrom build/amd64-gcc/belka.iso -serial mon:stdio
```

- Belka's virtio drivers are for modern devices only, hence
  `disable-legacy=on`.
- `-serial mon:stdio` is the console, with the QEMU monitor on Ctrl-A C.
- `-no-reboot` keeps a triple fault visible rather than letting it loop.
- Run `/sbin/e2fsck -p build/root-gcc.img` first if the last run did
  not end in `halt`.

## 8. When something goes wrong

- **Nothing after the firmware, or "OVMF_CODE_4M.fd: No such file":**
  the firmware is missing or elsewhere; see "Firmware in another place".
- **Everything is very slow:** QEMU is using TCG; check `/dev/kvm`
  access and `info kvm` in the monitor.
- **"Read-only file system" in Belka:** the last run did not end in
  `halt`. `halt` now, then `make run` repairs the image.
- **"muon: not found", or the C library fails to configure:** muon or
  samurai is missing or too old; see "muon and samurai".
- **The submodule update fails for `ksenon-utils`:** it needs ssh access
  to GitHub.
- **A test says FAIL:** read its serial log. A line such as
  `boottest: 1071: <check>` names the check that failed, by its line in
  the test's source.
