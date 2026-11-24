# The file system hierarchy

**Status:** adopted 2026-11-23; the image follows it since 2026-11-24,
with the gaps listed in "Where the tree stands" at the end.

This is the contract for where things go in a Belka system: which
directories there are, who owns each, what survives a reboot, and what
resetting the system to its defaults removes.

## 1. Principles

Belka keeps three kinds of software apart:

1. **The base operating system:** its programs and libraries under
   `/usr`, the few static ones in `/sbin`, its configuration in `/etc`,
   and its records and state in `/var/adm` and `/var/db`. It is
   installed and updated as a whole, never by the package manager.
2. **Software installed for the whole system:** under the `/opt`
   prefix, with its configuration in `/etc/opt` and its state and logs
   in `/var/opt`. This is what the package manager manages, and all it
   manages.
3. **Software a user installs for themself:** under
   `/opt/people/USER`, with its configuration in `/etc/opt/people/USER`
   and its state in `/var/opt/people/USER`.

`USER` stands for an account's name.

The point of the split: **the system goes back to its defaults by
removing what is under `/opt`, `/etc/opt` and `/var/opt`, and putting
back `/etc`'s files from their defaults.** Nothing that is installed
afterwards touches the base, so nothing of the base has to be found and
undone.

`/`, `/usr` and `/etc` are on one file system. `/usr` is not mounted
separately and is not required to be mountable separately; there are
no BSD-style slices.

## 2. The directories

| Path | What it is for |
| --- | --- |
| `/adm` | The superuser's home directory (there is no `/root`). |
| `/bin` | A symbolic link to `usr/bin`, for the paths everything expects (`/bin/sh`). |
| `/dev` | Devices, made by the kernel's devfs. Names are broadly System V's. |
| `/etc` | The base system's configuration. |
| `/etc/defaults` | The base system's configuration as shipped: what a reset puts back. |
| `/etc/opt` | Configuration for software installed for the whole system. |
| `/etc/opt/people/USER` | Configuration for software a user installed; the user's own. |
| `/mnt` | Where other file systems are mounted. |
| `/opt` | The prefix for software installed for the whole system, as `/usr/local` elsewhere: `/opt/bin`, `/opt/sbin`, `/opt/lib`, `/opt/include`, `/opt/share` and so on. One merged prefix, not a tree per package. The administrator's. |
| `/opt/people/USER` | A user's own installation prefix, laid out as `/opt` is. |
| `/proc` | Processes (procfs): Belka's own interface, after IRIX's, not Linux's. |
| `/sbin` | Statically linked programs for starting and recovering the system: init, the shell, mount and umount, halt and reboot, netcfg, rcorder, ed, and fsck when there is one. They run when the shared C library, the runtime linker or `/usr` is damaged. |
| `/stand` | Reserved for the kernel and its boot files, if the system is installed to a disk. |
| `/tmp` | Scratch space, a tmpfs: empty at every boot. |
| `/usr/bin` | The base system's commands. |
| `/usr/sbin` | The base system's administrative commands. Being here grants nothing: authorisation is the program's own to check. |
| `/usr/include` | The base system's headers. |
| `/usr/lib` | Reserved for a 32-bit (UnixWare) personality, whose programs name `/usr/lib/libc.so.1` as their interpreter. |
| `/usr/lib64` | The base system's 64-bit libraries: `libc.so.1`, `ld.so.1`, `libc.a`. Dynamic programs name `/usr/lib64/ld.so.1`. |
| `/usr/libexec` | Programs the base system runs, not people: getty, dhcpcd's hook. |
| `/usr/people` | Home directories, `/usr/people/USER`, as on IRIX. |
| `/usr/share` | The base system's architecture-independent files: manual pages, terminfo, licences. |
| `/var` | Data that changes, kept across reboots unless said otherwise below. |
| `/var/adm` | The base system's logs and administrative records (`utmpx`, `wtmpx`, `lastlogx`). |
| `/var/db` | The base system's persistent state (dhcpcd's leases and DUID). |
| `/var/log` | A symbolic link to `opt/log`, for software that writes there. |
| `/var/opt` | State of software installed for the whole system, `/var/opt/PACKAGE`; and its logs, `/var/opt/log`. |
| `/var/opt/people/USER` | State of software a user installed; the user's own. |
| `/var/run` | Process IDs, sockets and the like, a tmpfs: empty at every boot. |
| `/var/tmp` | Scratch space that is kept across reboots. |

`/opt/people`, `/etc/opt/people` and `/var/opt/people` keep users apart
from the shared prefix's own directories and from packages: a user may
be called `bin`, `lib` or `log` without meeting `/opt/bin`, `/opt/lib`
or `/var/opt/log`.

## 3. Who owns what

### The base system

The base system puts its commands in `/usr/bin` and `/usr/sbin` (the
static few in `/sbin`), its libraries in `/usr/lib64`, its headers in
`/usr/include`, and its other files in `/usr/share` and `/usr/libexec`.
Everything there is root's. It is meant not to change in ordinary
running, except the homes under `/usr/people`: that is a matter of
ownership and permissions, checked by a manifest (below), not of a
read-only mount, since `/usr/people` is on the same file system.

The base system's configuration is in `/etc`. Each file it ships is
also in `/etc/defaults`, unchanged, and `/etc` holds the local copy or
only the local overrides (as `/etc/rc.conf` does `/etc/defaults/rc.conf`).
The accounts (`passwd`, `shadow`, `group`) are the system's, not
defaults: a reset keeps them.

### Software for the whole system

The administrator owns `/opt`, `/etc/opt` and `/var/opt`. Software
installed there does not change `/usr`, `/sbin` or the base's `/etc`
files to install itself. Its logs go in `/var/opt/log` (`/var/log`),
its other state in `/var/opt/PACKAGE`.

### Software for one user

Each user may have three trees of their own:

```text
/opt/people/USER        programs, libraries and other installed files
/etc/opt/people/USER    their configuration
/var/opt/people/USER    their state
```

The user owns them; `/opt/people`, `/etc/opt/people` and
`/var/opt/people` stay root's. No user may write to another's trees or
to the shared prefix. These are apart from the user's home,
`/usr/people/USER`, and from the dotfiles in it.

## 4. Search paths and privilege

- **Libraries:** the base system's runtime linker looks only in
  `/usr/lib64`. Programs under `/opt` find their libraries through
  their own RUNPATH (`/opt/lib`, or `/opt/people/USER/lib`), so nothing
  under `/opt` can stand in for a base library, and removing `/opt`
  cannot break the base.
- **Commands:** a user's PATH puts the base first, then the shared
  prefix, then their own:
  `/usr/bin:/opt/bin:/opt/people/USER/bin`, with the `sbin`
  directories for those who administer. The superuser's PATH has no
  directory a user can write to: `/sbin:/usr/sbin:/usr/bin:/opt/sbin:/opt/bin`.
- **Trust:** a privileged program trusts no code or configuration for
  being under `/opt`, `/etc/opt` or a user's tree.

## 5. Going back to the defaults

A reset, when there is a tool for it, will:

1. remove everything under `/opt`, `/etc/opt` and `/var/opt`, users'
   trees included (homes in `/usr/people` are kept);
2. put each file in `/etc` back from `/etc/defaults`, except the
   accounts;
3. check the base system against its manifest, and put back what
   differs.

The **manifest** is a list, shipped with each release, of every file of
the base system with its owner, mode and checksum; a `verify` command
reports what differs from it. The base system is updated by replacing
it whole, in step with its manifest.

## 6. Accounts

New accounts' homes are `/usr/people/USER`; root's is `/adm`. The
account tools (`useradd`, `userdel` and the rest) are to know all of an
account's paths:

```text
/usr/people/USER
/opt/people/USER
/etc/opt/people/USER
/var/opt/people/USER
```

Belka is to keep a history of deleted accounts, so that a deleted
account's UID is not given to a new one, which would hand it the files
left behind. How that history is kept, how UIDs are chosen, what
restoring an account does, and how the administrator overrides it, are
to be decided; so is the same for GIDs, and what to do with file systems
and backups whose owners come from elsewhere.

## 7. Example

```text
/
├── adm/                    the superuser's home
├── bin -> usr/bin
├── dev/
├── etc/
│   ├── defaults/           as shipped
│   └── opt/
│       ├── nginx/
│       └── people/alice/
├── mnt/
├── opt/
│   ├── bin/  lib/  share/
│   └── people/alice/
│       └── bin/  lib/  share/
├── proc/
├── sbin/                   static: init, sh, mount, halt, netcfg, ed, ...
├── tmp/                    tmpfs
├── usr/
│   ├── bin/  include/  lib64/  libexec/  sbin/  share/
│   └── people/alice/       alice's home
└── var/
    ├── adm/  db/  tmp/
    ├── log -> opt/log
    ├── opt/
    │   ├── log/
    │   ├── nginx/
    │   └── people/alice/
    └── run/                tmpfs
```

## 8. Still open

1. The reset and `verify` tools, and the manifest's format.
2. The account history (section 6).
3. Whether `/stand` is used, and what it holds.
4. The package manager, and how it records what it installed.

## 9. Where the tree stands

The image (`tools/mkroot.sh`) follows this since 2026-11-24:

- **`/usr/bin`** has what was in `/bin` (mksh as `sh`, tcsh, the Ksenon
  utilities, ps, hostname, stty, gzip, pax, ed), and `/bin` is the link.
  Paths such as `/bin/sh` in `/etc/passwd`, `/etc/shells` and imported
  programs still work through it, and stay.
- **`/sbin`** has init, mount and umount, halt and reboot, netcfg and
  rcorder, and copies of `sh` and `ed`, all static. dhcpcd is
  `/usr/sbin/dhcpcd`, and its hook `/usr/libexec/dhcpcd-run-hooks`.
- **`/usr/sbin/nologin`**, new, is the system accounts' shell; it was
  named `/sbin/nologin` before, and there was no such program.
- **Homes:** root's is `/adm` (mode 0700); `useradd` makes new ones in
  `/usr/people`; the test image's users are in `/usr/people/test`.
  `/root`, `/home` and `/libexec` are gone.
- **New:** `/opt/people`, `/etc/opt/people`, `/var/opt/people`,
  `/var/opt/log` (with `/var/log` the link), `/var/db`, and a `/var/run`
  tmpfs (`/etc/vfstab`).
- **PATH** (`/etc/profile`): `/sbin:/usr/sbin:/usr/bin:/opt/sbin:/opt/bin`
  for root, `/usr/bin:/opt/bin:/opt/people/USER/bin` for others;
  `/etc/rc`'s is `/sbin:/usr/sbin:/usr/bin`.
- **The image is made with umask 022,** so that the base system's
  directories are root's alone, whatever the host's umask.

Not yet done:

- **`/usr/lib/gcc`:** the native GCC keeps its own files there, as its
  build lays them out. It is to go under `/usr/lib64` when the native
  compiler is next built.
- **`/etc/defaults`** has only `rc.conf`; the other files shipped in
  `/etc` are to be copied there as they are.
- The reset and `verify` tools, the manifest, and the account history
  (section 8).
