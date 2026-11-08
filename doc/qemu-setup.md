# QEMU Test Environment

How to prepare the development host (Debian 13) so that Belka can be built
and booted under QEMU.

## Packages

QEMU, GCC, Clang, GNU make, and git are already installed. The following
are still needed:

```sh
sudo apt install ovmf lld xorriso mtools
```

- `ovmf`: UEFI firmware for QEMU (`/usr/share/OVMF/OVMF_CODE_4M.fd`).
- `lld`: the LLVM linker, for Clang builds.
- `xorriso` and `mtools`: to build bootable ISO and FAT images.

## KVM access

The build user must be able to open `/dev/kvm`:

```sh
sudo usermod -aG kvm devel-claude
```

Start a new login session afterwards. Check with `id`; the list should
include `kvm`.

If this host is itself a virtual machine, nested virtualisation must be
enabled on the hypervisor and the VM's CPU type must be `host`. Without
KVM, QEMU falls back to TCG, which works but is slow.

## Reference command line

The build will provide `make run`. It amounts to:

```sh
qemu-system-x86_64 \
    -machine q35,accel=kvm:tcg -cpu max -smp 4 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -cdrom belka.iso \
    -serial stdio -display none -no-reboot -no-shutdown
```

`-serial stdio` is how the kernel is observed and how automated tests read
its output. `-no-reboot` keeps a triple fault visible instead of looping.
