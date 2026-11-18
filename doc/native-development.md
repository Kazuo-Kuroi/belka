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
symbol indexes. There is no native C compiler or assembler yet.

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

## Remaining compiler bootstrap

GCC 14 or newer is the initial compiler direction, as required by mlibc.
PathScale is a future compiler and is not a bootstrap dependency.

1. Port an amd64 assembler and the remaining object tools needed by GCC.
   Keep illumos ld as the system link-editor.
2. Add a GCC Belka target: configure recognition, System V ELF ABI,
   predefined target macros, start files, library and interpreter paths,
   TLS, and driver specifications for the illumos link-editor.
3. Cross-build the target compiler and its GMP, MPFR and MPC dependencies
   against Belka. Build the C and C++ front ends and the target C++ runtime
   needed by a modern GCC bootstrap. Test the driver and generated code.
4. Recompile the installed illumos sources inside Belka with that compiler,
   using native gmake, m4 and sgsmsg. Relink and rerun the linker tests.
5. Bootstrap GCC again inside Belka, then build mlibc and the userland.
   muon and samurai also need native ports for mlibc's current build.

No posix_spawn dependency is introduced: GNU make is configured to use
fork and exec. Networking is not part of these stages.
