# Native development tools

The host build requires GNU make 4.3 or newer for grouped generated targets.

Belka supplies NetBSD bmake, m4 and tsort, Berkeley yacc, and flex. GNU
make 4.4.1 is available as gmake and make because Belka's and GCC's build
systems require it. GNU make keeps its GPL licence; the other tools keep
their individual BSD, ISC or MIT-style notices. The install command is
the MIT/X11 install-sh script, rather than NetBSD's hashing and mtree
implementation. Its strip option needs a separately installed strip tool.

The native illumos ld, ar, nm and sgsmsg use the existing port's MIT
compatibility layer. The root image includes their CDDL sources and
licence under /usr/src/illumos-ld. ranlib invokes ar -s to rebuild archive
symbol indexes. The optional compiler package adds GCC 14.2.0 and GNU
as 2.43; illumos ld remains the system link-editor.

## Linker bootstrap

/usr/include and /usr/lib64 contain libc headers, shared and static libc,
start files, compiler runtime builtins, and the lex runtime library
(libl.a, also available as libfl.a). The linker archives and its initial
objects are under /usr/lib64/illumos-ld.

Inside Belka, in a writable directory:

```
gmake -f /usr/src/illumos-ld/relink.mk
mv ld.stage2 ld.bootstrap
gmake -f /usr/src/illumos-ld/relink.mk LD=./ld.bootstrap
cmp ld.bootstrap ld.stage2
```

This verifies native relinking and a reproducible second link, using
objects initially cross-compiled on the host. It is not yet a source
self-host. Use the same output filename for the comparison because the
link-editor records it in the ELF symbol table.

The boot tests also exercise parallel make jobs, scanner and parser
generation, archive indexing and extraction, relocatable links, and
running statically and dynamically linked programs. Native m4 regenerates
libelf's translator and compares it with the host-generated source;
native sgsmsg generates an archive-tool message header and C table.

## Native compiler package

Build the optional compiler package on the host:

```
make native-compiler
make test-native-compiler
make root-image
```

The first target downloads the releases listed with SHA-256 hashes in
`tools/toolchain/sources`, verifies them, and applies the small target ports
under `contrib/gcc` and `contrib/binutils`. Sources stay in
`build/toolchain-src`; cached archives stay in `build/distfiles`. Ordinary
kernel builds do not download a compiler. Allow several gigabytes of disk
space and a substantial first build. `NATIVE_JOBS` controls parallelism.

The build first produces a host cross compiler and target runtime, then
cross-builds the native C and C++ front ends and assembler. GMP 6.2.1,
MPFR 4.1.0 and MPC 1.2.1 are built with the compiler and installed as
static libraries with headers. Upstream licences accompany the package.
GCC and GNU as retain their GNU licences.

The package is staged in `build/native-compiler-sdk` and copied into root
images when present. Compiler images use 512 MB instead of the normal
128 MB. Remove the staging directory to omit the optional package.
Native commands include `gcc`, `cc`, `g++`, `c++` and `as`. GCC uses
/usr/lib64 for system libraries and /usr/lib64/ld.so.1 as interpreter.
Only amd64 LP64 is enabled. The initial runtime is static libgcc and
libstdc++; shared application libraries remain supported. Multilib, LTO,
plugins, NLS and optional GCC runtime projects are disabled.

GCC's exception-registration CRT objects coexist with Belka's libc array
boundaries. Its executable finalizer calls the executable's C++ destructors
before unregistering unwind tables; shared libraries omit executable-only
array markers. The loader supports the narrow amd64 relocations emitted by
GCC and restores non-writable segment permissions after text relocations.
Native tools use fork and exec; posix_spawn remains disabled. They avoid
sbrk-based heap-size probes. Physical-memory sysconf queries return an
indeterminate value until a kernel query exists.

The compiler acceptance script exercises C99, static linking, compiler
pipes, shared-object TLS, C++17 exceptions and threads, multiprecision
libraries and diagnostic exit status. Run it with `make test-native-compiler`.

## Build the linker from source

Inside Belka with the compiler package installed:

```
gmake -C /usr/src/illumos-ld/port O=/tmp/native-ld \
    HOSTCC=gcc LDLIBS= CFLAGS=-O2 -j2
/tmp/native-ld/ld -64 -r -o /tmp/native-link.o /usr/lib64/crt1.o
```

This builds the generators, libraries and linker from their C sources,
using native gcc, as, gmake, m4, sgsmsg and ar. The resulting linker has
passed relocatable links, static and dynamic executable links, a shared
library and its client, and relinking the linker itself. The earlier bootstrap
relink recipe remains useful for comparing successive links of the same
object set.

## Remaining compiler bootstrap

Full GCC source self-bootstrap is a later acceptance stage. Native muon
and samurai are also needed to build mlibc with its current build system.
PathScale is not a bootstrap dependency. Networking is outside these stages.
