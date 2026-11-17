#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Build a root file system image (ext2) that boots to init and logins:
#
#   /sbin/init, reboot, halt    /usr/libexec/getty    /usr/bin/login
#   /bin/sh (mksh), /bin/mksh   /bin/ls, cat, cp ... (ksenon-utils)
#   /etc from the tree's etc/, with /etc/mnttab a link to /dev/mnttab
#   /bin/ps (Belka's), /dev and /proc (devfs and procfs go on them),
#   /tmp, /root
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

mkdir -p "$src/bin" "$src/sbin" "$src/usr/bin" "$src/usr/libexec" \
    "$src/usr/tests" "$src/usr/lib64" "$src/etc/defaults" "$src/dev" \
    "$src/proc" "$src/tmp" "$src/root" "$src/home" "$src/var/adm" \
    "$src/var/tmp"

install_prog() {        # program destination
        $STRIP --strip-debug -o "$src/$2" "$sys/$1"
}
install_prog init sbin/init
install_prog reboot sbin/reboot
ln "$src/sbin/reboot" "$src/sbin/halt"
install_prog getty usr/libexec/getty
install_prog login usr/bin/login
install_prog mksh bin/mksh
ln "$src/bin/mksh" "$src/bin/sh"

# The utilities (ksenon-utils): cp and ln are mv, chgrp is chown, [ is
# test.
for prog in "$sys"/xn/*; do
        name=${prog##*/}
        case $name in
        *.a)    continue ;;
        esac
        # GNU strip mangles the illumos link-editor's dynamic objects.
        if [ "$name" = mv ]; then
                install_prog "xn/$name" "bin/$name"
        else
                cp "$prog" "$src/bin/$name"
        fi
done
ln "$src/bin/mv" "$src/bin/cp"
ln "$src/bin/mv" "$src/bin/ln"
ln "$src/bin/chown" "$src/bin/chgrp"
ln "$src/bin/test" "$src/bin/["
cp "$sys/ps" "$src/bin/ps"            # dynamic: not stripped
# terminfo: the programs, the descriptions' source (/etc/rc compiles
# the database), and ed.
for prog in tic infocmp tput passwd; do
        cp "$sys/$prog" "$src/usr/bin/$prog"    # dynamic: not stripped
done
cp usr/bin/clear/clear.sh "$src/usr/bin/clear"
# sed, grep and awk (NetBSD's); egrep and fgrep are grep.
for prog in sed grep awk; do
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
install_prog ed bin/ed
mkdir -p "$src/usr/share/misc"
cp contrib/netbsd/share/terminfo/terminfo "$src/usr/share/misc/terminfo"
cp "$libdir/ld.so.1" "$libdir/libc.so.1" "$src/usr/lib64/"

for f in rc rc.conf ttys passwd shadow group shells profile \
    defaults/rc.conf; do
        cp "etc/$f" "$src/etc/$f"
done
# The mount table is the kernel's (devfs).
ln -s ../dev/mnttab "$src/etc/mnttab"

if [ $test -eq 1 ]; then
        install_prog boottest usr/tests/boottest
        cp usr/tests/utiltest.sh "$src/usr/tests/utiltest.sh"
        # ksenon-utils' own suite, over a directory of just its 45
        # utilities and their links, as its tests/run.sh expects (../cmd).
        mkdir -p "$src/usr/tests/xn/cmd" "$src/usr/tests/xn/tests"
        cp contrib/ksenon-utils/tests/run.sh "$src/usr/tests/xn/tests/run.sh"
        for prog in "$sys"/xn/*; do
                case ${prog##*/} in *.a) continue ;; esac
                ln "$src/bin/${prog##*/}" "$src/usr/tests/xn/cmd/${prog##*/}"
        done
        for l in cp ln; do
                ln "$src/bin/mv" "$src/usr/tests/xn/cmd/$l"
        done
        ln "$src/bin/chown" "$src/usr/tests/xn/cmd/chgrp"
        ln "$src/bin/test" "$src/usr/tests/xn/cmd/["
        # crypt("belka-test", "$sha1$24680$belkatst$"); boottest logs in
        # with it, so Belka's crypt checks it.
        hash='$sha1$24680$belkatst$XycoewmXxvwRJYfMFY3Jpt0BG5Li'
        echo "test:x:100:100:Boot test:/home/test:/bin/sh" \
            >> "$src/etc/passwd"
        echo "test:$hash:$(($(date +%s) / 86400))::::::" >> "$src/etc/shadow"
        mkdir -p "$src/home/test"
        cat > "$src/etc/rc.local" <<'EOF'
# The boot test: in the background, so that init goes on to multi-user.
/usr/tests/boottest test belka-test 100 /home/test &
EOF
fi

rm -f "$img"
$FAKEROOT sh -c '
        set -e
        src=$1
        chown -R 0:0 "$src"
        chmod 0755 "$src"/bin/* "$src"/sbin/* "$src"/usr/libexec/* \
            "$src"/usr/bin/*
        chmod 4555 "$src/usr/bin/login" "$src/usr/bin/passwd"
        chmod 0400 "$src/etc/shadow"
        chmod 1777 "$src/tmp" "$src/var/tmp" "$src/var/tmp/vi.recover"
        chmod 0700 "$src/root"
        if [ -d "$src/home/test" ]; then
                chown 100:100 "$src/home/test"
        fi
        "$2" -q -F -t ext2 -b 4096 -N 4096 -L belkaroot -d "$src" "$3" 128M
' sh "$src" "$MKE2FS" "$img"
