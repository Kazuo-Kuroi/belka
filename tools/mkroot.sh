#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Build a root file system image (ext2) that boots to init and logins:
#
#   laid out as doc/design/hierarchy.md says:
#   /sbin: init, reboot and halt, mount and umount, rcorder, netcfg, and
#     copies of sh and ed, all static, to start and recover the system
#   /usr/bin/sh (mksh), mksh, tcsh, ls, cat, cp ... (ksenon-utils), ps
#     (Belka's), and the rest; /bin a link to usr/bin
#   /usr/libexec/getty, /usr/bin/login, /usr/sbin/nologin
#   /etc from the tree's etc/, with /etc/mnttab a link to /dev/mnttab
#   /dev and /proc (devfs and procfs go on them), /tmp and /var/run
#     (tmpfs), /adm (root's home), /usr/people (homes)
#   /opt, /etc/opt and /var/opt, empty but for their people/ (and
#     /var/opt/log, which /var/log links to)
#   /usr/sbin/dnscache, /usr/bin/dnsip, dnsname, dnsq, dnsqr (djbdns),
#     with dnscache's root in /etc/dnscache/root
#   /usr/sbin/dhcpcd (NetBSD's DHCP client) and its hook,
#     /usr/libexec/dhcpcd-run-hooks; its state in /var/db/dhcpcd
#   /usr/sbin/tcpserver, tcprules, tcprulescheck, /usr/bin/tcpclient
#     (ucspi-tcp), for /etc/rc.d/inetd and /etc/inetd.conf
#   Dropbear's SSH programs under OpenSSH's names: /usr/sbin/sshd,
#     /usr/bin/ssh, ssh-keygen and scp, and dropbearconvert
#
# Everything is owned by root, and login is set-user-ID, through
# fakeroot(1). The password hashes are in /etc/shadow (mode 0400);
# root has no password.
#
# With -t, the image is the boot test's: user "test" (uid 100, password
# "belka-test", its hash written here) and an /etc/rc.local that runs
# /usr/tests/boottest in the background as the system comes up.
#
# The utilities but mv are dynamic, so /usr/lib64 gets the runtime
# linker and libc.so.1 from libdir.
#
# usage: mkroot.sh [-t] image sysdir libdir
#

set -eu
# Not the host's: what is made here is root's, writable by root alone.
umask 022

test=0
if [ "${1:-}" = -t ]; then
        test=1
        shift
fi
if [ $# -ne 3 ]; then
        echo "usage: $0 [-t] image sysdir libdir" >&2
        exit 1
fi
img=$1
sys=$2
libdir=$3
MKE2FS=${MKE2FS:-/sbin/mke2fs}
FAKEROOT=${FAKEROOT:-fakeroot}
STRIP=${STRIP:-strip}

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

mkdir -p "$src/sbin" "$src/mnt" "$src/usr/bin" "$src/usr/sbin" "$src/usr/libexec" \
    "$src/usr/tests" "$src/usr/lib64" "$src/etc/defaults" "$src/dev" \
    "$src/proc" "$src/tmp" "$src/adm" "$src/usr/people" "$src/var/adm" \
    "$src/var/db" "$src/opt/people" "$src/etc/opt/people" \
    "$src/var/opt/log" "$src/var/opt/people" \
    "$src/var/tmp" "$src/var/run"

install_prog() {        # program destination
        $STRIP --strip-debug -o "$src/$2" "$sys/$1"
}
install_prog accounts usr/sbin/useradd
for name in usermod userdel groupadd groupmod groupdel acctrecover; do
        ln "$src/usr/sbin/useradd" "$src/usr/sbin/$name"
done
install_prog init sbin/init
install_prog reboot sbin/reboot
ln "$src/sbin/reboot" "$src/sbin/halt"
install_prog mount sbin/mount
install_prog rcorder sbin/rcorder
install_prog netcfg sbin/netcfg
ln "$src/sbin/mount" "$src/sbin/umount"
install_prog getty usr/libexec/getty
install_prog login usr/bin/login
install_prog chsh usr/bin/chsh
install_prog mksh usr/bin/mksh
install_prog tcsh usr/bin/tcsh
# djbdns' caching resolver and its client tools (contrib/djbdns).
install_prog dnscache usr/sbin/dnscache
for prog in dnsip dnsname dnsq dnsqr dnsmx dnstxt dnsipq dnstrace dnsfilter \
    xdig; do
        install_prog $prog usr/bin/$prog
done
# dnstracesort, djbdns' script, with sort's keys in -k form, which
# Belka's sort takes (it has not the old +pos -pos form).
{ echo "#!/bin/sh"; sed "s/sort -t: +0 -2 +4 +3 -4 +2 -3/sort -t: -k1,2 -k5 -k4,4 -k3,3/" \
    contrib/djbdns/dnstracesort.sh; } > "$src/usr/bin/dnstracesort"
# host, dig and drill: Belka's wrappers over dnsqr (usr/bin/dnstools).
cp usr/bin/dnstools/host.sh "$src/usr/bin/host"
cp usr/bin/dnstools/dig.sh "$src/usr/bin/dig"
ln "$src/usr/bin/dig" "$src/usr/bin/drill"
# ping (NetBSD's) and traceroute (Belka's), set-user-ID root below for
# their raw sockets; whois (NetBSD's).
install_prog ping usr/bin/ping
install_prog traceroute usr/bin/traceroute
cp "$sys/whois" "$src/usr/bin/whois"
mkdir -p "$src/usr/share/licenses/djbdns"
cp contrib/djbdns/README.belka "$src/usr/share/licenses/djbdns/"
# dhcpcd, NetBSD's DHCP client (contrib/dhcpcd), its state and licence.
install_prog dhcpcd usr/sbin/dhcpcd
cp usr/sbin/dhcpcd/dhcpcd-run-hooks "$src/usr/libexec/dhcpcd-run-hooks"
mkdir -p "$src/usr/share/licenses/dhcpcd" "$src/var/db/dhcpcd"
cp contrib/dhcpcd/LICENSE contrib/dhcpcd/README.belka \
    "$src/usr/share/licenses/dhcpcd/"
# ucspi-tcp's tcpserver and its tools (contrib/ucspi-tcp), for inetd.
for prog in tcpserver tcprules tcprulescheck; do
        install_prog $prog usr/sbin/$prog
done
install_prog tcpclient usr/bin/tcpclient
mkdir -p "$src/usr/share/licenses/ucspi-tcp" "$src/etc/inetd"
cp contrib/ucspi-tcp/README.belka "$src/usr/share/licenses/ucspi-tcp/"
# Dropbear (contrib/dropbear), under OpenSSH's names; its host keys
# are made in /etc/dropbear at the first boot (/etc/rc.d/sshd).
install_prog dropbear usr/sbin/sshd
install_prog dbclient usr/bin/ssh
install_prog dropbearkey usr/bin/ssh-keygen
install_prog dropbearconvert usr/bin/dropbearconvert
install_prog scp usr/bin/scp
mkdir -p "$src/usr/share/licenses/dropbear" "$src/etc/dropbear"
chmod 700 "$src/etc/dropbear"
cp contrib/dropbear/LICENSE contrib/dropbear/README.belka \
    "$src/usr/share/licenses/dropbear/"
mkdir -p "$src/usr/share/licenses/tcsh"
cp contrib/tcsh/Copyright contrib/tcsh/NOTICE "$src/usr/share/licenses/tcsh/"
ln "$src/usr/bin/mksh" "$src/usr/bin/sh"
# /sbin's own static copies, apart from /usr's, for recovery.
install_prog mksh sbin/sh
install_prog nologin usr/sbin/nologin

# The utilities (ksenon-utils): cp and ln are mv, chgrp is chown, [ is
# test.
for prog in "$sys"/xn/*; do
        name=${prog##*/}
        case $name in
        *.a)    continue ;;
        esac
        # GNU strip mangles the illumos link-editor's dynamic objects.
        if [ "$name" = mv ]; then
                install_prog "xn/$name" "usr/bin/$name"
        else
                cp "$prog" "$src/usr/bin/$name"
        fi
done
ln "$src/usr/bin/mv" "$src/usr/bin/cp"
ln "$src/usr/bin/mv" "$src/usr/bin/ln"
ln "$src/usr/bin/chown" "$src/usr/bin/chgrp"
ln "$src/usr/bin/test" "$src/usr/bin/["
cp "$sys/ps" "$src/usr/bin/ps"            # dynamic: not stripped
cp "$sys/hostname" "$src/usr/bin/hostname"
cp "$sys/stty" "$src/usr/bin/stty"
cp "$sys/gzip" "$src/usr/bin/gzip"
for name in gunzip gzcat zcat; do
        ln "$src/usr/bin/gzip" "$src/usr/bin/$name"
done
mkdir -p "$src/usr/share/licenses/gzip"
cp contrib/netbsd/usr.bin/gzip/NOTICE "$src/usr/share/licenses/gzip/"
mkdir -p "$src/usr/share/licenses/stty"
cp contrib/netbsd/bin/stty/NOTICE "$src/usr/share/licenses/stty/"
# terminfo: the programs, the descriptions' source (/etc/rc compiles
# the database), and ed.
for prog in tic infocmp tput passwd su less mandoc; do
        cp "$sys/$prog" "$src/usr/bin/$prog"    # dynamic: not stripped
done
# Development tools remain unstripped: GNU strip damages dynamic illumos ELF.
for prog in bmake gmake m4 tsort yacc lex ld ar nm samu muon; do
        cp "$sys/$prog" "$src/usr/bin/$prog"
done
mkdir -p "$src/usr/share/licenses/samurai" "$src/usr/share/licenses/muon"
cp contrib/samurai/LICENSE "$src/usr/share/licenses/samurai/"
cp -R contrib/muon/LICENSES/. "$src/usr/share/licenses/muon/"
ln "$src/usr/bin/gmake" "$src/usr/bin/make"
ln "$src/usr/bin/samu" "$src/usr/bin/ninja"
ln "$src/usr/bin/yacc" "$src/usr/bin/byacc"
ln "$src/usr/bin/lex" "$src/usr/bin/flex"
cp contrib/gmake/build-aux/install-sh "$src/usr/bin/install"
chmod 755 "$src/usr/bin/install"
cp usr/bin/ranlib/ranlib.sh "$src/usr/bin/ranlib"
chmod 755 "$src/usr/bin/ranlib"
cp "$sys/illumos-ld/sgsmsg" "$src/usr/libexec/sgsmsg"
mkdir -p "$src/usr/share/mk"
cp usr/share/mk/sys.mk "$src/usr/share/mk/sys.mk"
for prog in make m4 tsort; do
        mkdir -p "$src/usr/share/licenses/$prog"
        cp "contrib/netbsd/usr.bin/$prog/NOTICE" "$src/usr/share/licenses/$prog/"
done
mkdir -p "$src/usr/share/licenses/gmake" "$src/usr/share/licenses/byacc" \
    "$src/usr/share/licenses/flex"
cp contrib/gmake/COPYING "$src/usr/share/licenses/gmake/"
cp contrib/netbsd/external/bsd/byacc/dist/LICENSE \
    contrib/netbsd/external/bsd/byacc/dist/NOTICE "$src/usr/share/licenses/byacc/"
cp contrib/netbsd/external/bsd/flex/dist/COPYING \
    contrib/netbsd/external/bsd/flex/dist/COPYING.LIB \
    contrib/netbsd/external/bsd/flex/dist/NOTICE "$src/usr/share/licenses/flex/"
ln "$src/usr/bin/less" "$src/usr/bin/more"
for name in man apropos whatis makewhatis; do
        ln "$src/usr/bin/mandoc" "$src/usr/bin/$name"
done
cp "$sys/pax" "$src/usr/bin/pax"
ln "$src/usr/bin/pax" "$src/usr/bin/tar"
ln "$src/usr/bin/pax" "$src/usr/bin/cpio"

mkdir -p "$src/usr/share/man/man1" "$src/usr/share/man/man5" \
    "$src/usr/share/man/man7" "$src/usr/share/man/man8"
for section in 1 5 7 8; do
        for page in contrib/mandoc/*.$section; do
                case ${page##*/} in
                catman.8|mandocd.8|man.cgi.8|demandoc.1|soelim.1) continue ;;
                esac
                cp "$page" "$src/usr/share/man/man$section/"
        done
done
cp contrib/illumos-ld/usr/src/man/man1/ar.1 \
    contrib/illumos-ld/usr/src/man/man1/nm.1 \
    contrib/illumos-ld/usr/src/man/man1/ld.1 "$src/usr/share/man/man1/"
cp contrib/netbsd/external/bsd/flex/dist/doc/flex.1 "$src/usr/share/man/man1/lex.1"
ln "$src/usr/share/man/man1/lex.1" "$src/usr/share/man/man1/flex.1"
cp contrib/netbsd/usr.bin/make/make.1 "$src/usr/share/man/man1/bmake.1"
cp contrib/samurai/samu.1 "$src/usr/share/man/man1/samu.1"
ln "$src/usr/share/man/man1/samu.1" "$src/usr/share/man/man1/ninja.1"
cp contrib/gmake/doc/make.1 "$src/usr/share/man/man1/gmake.1"
# NetBSD's pages say /libexec; Belka's is /usr/libexec.
sed s,/libexec/dhcpcd,/usr/libexec/dhcpcd,g contrib/dhcpcd/src/dhcpcd.8 \
    > "$src/usr/share/man/man8/dhcpcd.8"
sed s,/libexec/dhcpcd,/usr/libexec/dhcpcd,g contrib/dhcpcd/src/dhcpcd.conf.5 \
    > "$src/usr/share/man/man5/dhcpcd.conf.5"
ln "$src/usr/share/man/man1/gmake.1" "$src/usr/share/man/man1/make.1"
# Dropbear's pages, also under the names its programs have here.
cp contrib/dropbear/manpages/dropbear.8 "$src/usr/share/man/man8/"
ln "$src/usr/share/man/man8/dropbear.8" "$src/usr/share/man/man8/sshd.8"
cp contrib/dropbear/manpages/dbclient.1 contrib/dropbear/manpages/dropbearkey.1 \
    contrib/dropbear/manpages/dropbearconvert.1 "$src/usr/share/man/man1/"
ln "$src/usr/share/man/man1/dbclient.1" "$src/usr/share/man/man1/ssh.1"
ln "$src/usr/share/man/man1/dropbearkey.1" "$src/usr/share/man/man1/ssh-keygen.1"
cp contrib/netbsd/usr.bin/m4/m4.1 contrib/netbsd/usr.bin/tsort/tsort.1 \
    contrib/netbsd/external/bsd/byacc/dist/yacc.1 "$src/usr/share/man/man1/"
cp contrib/netbsd/bin/stty/stty.1 contrib/netbsd/bin/pax/pax.1 \
    contrib/netbsd/bin/pax/tar.1 contrib/netbsd/bin/pax/cpio.1 \
    "$src/usr/share/man/man1/"
cp contrib/netbsd/external/bsd/less/dist/less.1 \
    "$src/usr/share/man/man1/less.1"
ln "$src/usr/share/man/man1/less.1" "$src/usr/share/man/man1/more.1"
sed -e 's/@PACKAGE_VERSION@/6.24.16/g' \
    -e 's/@PACKAGE_ORIGIN@/Astron/g' -e 's/@PACKAGE_DATE@/2025-07-09/g' \
    -e 's/@PACKAGE_ENGLISH_DATE@/July 9, 2025/g' \
    -e 's|@PACKAGE_BUGREPORT@|https://www.tcsh.org/|g' \
    -e 's|@PACKAGE_MAILLIST@|https://www.tcsh.org/|g' contrib/tcsh/tcsh.man.in \
    > "$src/usr/share/man/man1/tcsh.1"
cp usr/sbin/accounts/accounts.8 "$src/usr/share/man/man8/useradd.8"
cp usr/sbin/netcfg/netcfg.8 "$src/usr/share/man/man8/netcfg.8"
cp contrib/netbsd/sbin/ping/ping.8 "$src/usr/share/man/man8/ping.8"
cp contrib/netbsd/usr.bin/whois/whois.1 usr/bin/traceroute/traceroute.1 \
    usr/bin/xdig/xdig.1 usr/bin/dnstools/host.1 usr/bin/dnstools/dig.1 \
    "$src/usr/share/man/man1/"
ln "$src/usr/share/man/man1/dig.1" "$src/usr/share/man/man1/drill.1"
for name in usermod userdel groupadd groupmod groupdel acctrecover; do
        ln "$src/usr/share/man/man8/useradd.8" "$src/usr/share/man/man8/$name.8"
done
cp usr/bin/chsh/chsh.1 contrib/netbsd/usr.bin/gzip/gzip.1 \
    contrib/netbsd/usr.bin/diff/diff.1 contrib/netbsd/usr.bin/patch/patch.1 \
    "$src/usr/share/man/man1/"
for name in gunzip gzcat zcat; do
        ln "$src/usr/share/man/man1/gzip.1" "$src/usr/share/man/man1/$name.1"
done

for name in less mandoc pax zlib diff patch; do
        mkdir -p "$src/usr/share/licenses/$name"
done
cp contrib/netbsd/external/bsd/less/dist/LICENSE \
    "$src/usr/share/licenses/less/"
cp contrib/mandoc/LICENSE contrib/mandoc/NOTICE "$src/usr/share/licenses/mandoc/"
cp contrib/netbsd/bin/pax/NOTICE "$src/usr/share/licenses/pax/"
cp contrib/zlib/README "$src/usr/share/licenses/zlib/"
cp contrib/netbsd/usr.bin/diff/NOTICE "$src/usr/share/licenses/diff/"
cp contrib/netbsd/usr.bin/patch/NOTICE "$src/usr/share/licenses/patch/"

cp usr/bin/clear/clear.sh "$src/usr/bin/clear"
# sed, grep and awk (NetBSD's); egrep and fgrep are grep.
for prog in sed grep awk diff patch; do
        cp "$sys/$prog" "$src/usr/bin/$prog"
done
ln "$src/usr/bin/grep" "$src/usr/bin/egrep"
ln "$src/usr/bin/grep" "$src/usr/bin/fgrep"
# mktemp, seq, stat, find, xargs, getconf (NetBSD's); readlink is stat.
for prog in mktemp seq stat find xargs getconf; do
        cp "$sys/$prog" "$src/usr/bin/$prog"
done
ln "$src/usr/bin/stat" "$src/usr/bin/readlink"
# vi (nvi): ex and view are vi; its recovery files go in
# /var/tmp/vi.recover.
cp "$sys/vi" "$src/usr/bin/vi"
ln "$src/usr/bin/vi" "$src/usr/bin/ex"
ln "$src/usr/bin/vi" "$src/usr/bin/view"
mkdir -p "$src/var/tmp/vi.recover"
install_prog ed usr/bin/ed
install_prog ed sbin/ed
mkdir -p "$src/usr/share/misc"
cp contrib/netbsd/share/terminfo/terminfo "$src/usr/share/misc/terminfo"
cp "$libdir/ld.so.1" "$libdir/libc.so.1" "$src/usr/lib64/"
cp -R "$sys/sdk/." "$src/"
mkdir -p "$src/usr/share/licenses/mlibc" "$src/usr/share/licenses/compiler-runtime"
cp contrib/mlibc/LICENSE "$src/usr/share/licenses/mlibc/"
cp -R contrib/compiler-runtime-licenses/. "$src/usr/share/licenses/compiler-runtime/"

for f in rc rc.conf ttys passwd shadow group shells profile vfstab \
    hosts resolv.conf services defaults/rc.conf dhcpcd.conf inetd.conf; do
        cp "etc/$f" "$src/etc/$f"
done
# /bin is /usr/bin, and /var/log the shared prefix's logs.
ln -s usr/bin "$src/bin"
ln -s opt/log "$src/var/log"
# The mount table is the kernel's (devfs).
ln -s ../dev/mnttab "$src/etc/mnttab"
# The start-up scripts: NetBSD's rc.subr and some of its rc.d, and
# Belka's own (etc/rc.d).
mkdir -p "$src/etc/rc.d"
cp contrib/netbsd/etc/rc.subr "$src/etc/rc.subr"
cp contrib/netbsd/etc/rc.d/* etc/rc.d/* "$src/etc/rc.d/"
# The static routes, for rc.d/staticroute.
mkdir -p "$src/etc/routed"
cp etc/routed/static_route.conf "$src/etc/routed/static_route.conf"
# dnscache's root: the clients it serves, and where it starts from.
cp -R etc/dnscache "$src/etc/dnscache"
mkdir -p "$src/etc/dnscache/root/servers"

if [ $test -eq 1 ]; then
        install_prog boottest usr/tests/boottest
        install_prog exectest usr/tests/exectest
        cp usr/tests/exectest.c "$src/usr/tests/"
        cp usr/tests/utiltest.sh usr/tests/compiler.sh usr/tests/buildtools.sh "$src/usr/tests/"
        # ksenon-utils' own suite, over a directory of just its 45
        # utilities and their links, as its tests/run.sh expects (../cmd).
        mkdir -p "$src/usr/tests/xn/cmd" "$src/usr/tests/xn/tests"
        cp contrib/ksenon-utils/tests/run.sh "$src/usr/tests/xn/tests/run.sh"
        for prog in "$sys"/xn/*; do
                case ${prog##*/} in *.a) continue ;; esac
                ln "$src/usr/bin/${prog##*/}" "$src/usr/tests/xn/cmd/${prog##*/}"
        done
        for l in cp ln; do
                ln "$src/usr/bin/mv" "$src/usr/tests/xn/cmd/$l"
        done
        ln "$src/usr/bin/chown" "$src/usr/tests/xn/cmd/chgrp"
        ln "$src/usr/bin/test" "$src/usr/tests/xn/cmd/["
        # crypt("belka-test", "$sha1$24680$belkatst$"); boottest logs in
        # with it, so Belka's crypt checks it.
        hash='$sha1$24680$belkatst$XycoewmXxvwRJYfMFY3Jpt0BG5Li'
        echo "test:x:100:100:Boot test:/usr/people/test:/bin/sh" \
            >> "$src/etc/passwd"
        echo "test:$hash:$(($(date +%s) / 86400))::::::" >> "$src/etc/shadow"
        # A second user with the same password, for su's check of one.
        echo "test2:x:101:100:Boot test 2:/usr/people/test:/bin/sh" \
            >> "$src/etc/passwd"
        echo "test2:$hash:$(($(date +%s) / 86400))::::::" >> "$src/etc/shadow"
        # And a third, whose password ageing boottest changes.
        echo "test3:x:102:100:Boot test 3:/usr/people/test:/bin/sh" \
            >> "$src/etc/passwd"
        echo "test3:$hash:$(($(date +%s) / 86400))::::::" >> "$src/etc/shadow"
        mkdir -p "$src/usr/people/test"
        echo belkatest > "$src/etc/myname"     # the hostname script
        # A static route, which boottest looks for.
        printf '10.77.0.0/16\t10.0.2.2\t# boottest\n' \
            >> "$src/etc/routed/static_route.conf"
        # dnscache's root server: boottest's own, on the loopback.
        echo 127.0.0.53 > "$src/etc/dnscache/roots"
        cat > "$src/etc/rc.local" <<'EOF'
# The boot test: in the background, so that init goes on to multi-user.
/usr/tests/boottest test belka-test 100 /usr/people/test &
EOF
fi

image_size=128M
image_inodes=4096
if [ -f build/native-compiler-sdk/assembler.stamp ]; then
        cp -R build/native-compiler-sdk/usr/. "$src/usr/"
fi
if [ -f build/native-compiler-sdk/compiler.stamp ]; then
        image_size=512M
        image_inodes=32768
fi
if [ -f build/native-libc-sdk/libc.stamp ]; then
        cp -R build/native-libc-sdk/usr/. "$src/usr/"
fi
if [ -f build/native-object-sdk/objects.stamp ]; then
        cp -R build/native-object-sdk/usr/. "$src/usr/"
fi
if [ -f build/native-gcc-source-sdk/gcc-source.stamp ]; then
        cp -R build/native-gcc-source-sdk/usr/. "$src/usr/"
        image_size=8G
        image_inodes=262144
fi
rm -f "$img"
$FAKEROOT sh -c '
        set -e
        src=$1
        chown -R 0:0 "$src"
        chmod 0755 "$src"/sbin/* "$src"/usr/libexec/* \
            "$src"/usr/bin/* "$src"/usr/sbin/* "$src"/etc/rc.d/*
        chmod 4555 "$src/usr/bin/login" "$src/usr/bin/passwd" \
            "$src/usr/bin/su" "$src/usr/bin/chsh" \
            "$src/usr/bin/ping" "$src/usr/bin/traceroute"
        chmod 0400 "$src/etc/shadow"
        chmod 1777 "$src/tmp" "$src/var/tmp" "$src/var/tmp/vi.recover"
        chmod 0700 "$src/adm"
        if [ -d "$src/usr/people/test" ]; then
                chown 100:100 "$src/usr/people/test"
        fi
        "$2" -q -F -t ext2 -b 4096 -N "$5" -L belkaroot -d "$src" "$3" "$4"
' sh "$src" "$MKE2FS" "$img" "$image_size" "$image_inodes"
