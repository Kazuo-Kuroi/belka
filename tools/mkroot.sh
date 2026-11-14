#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# Build a root file system image (ext2) that boots to init and logins:
#
#   /sbin/init, reboot, halt    /usr/libexec/getty    /usr/bin/login
#   /bin/sh (mksh), /bin/mksh   /etc from the tree's etc/
#   /dev (devfs is mounted on it), /tmp, /root
#
# Everything is owned by root, and login is set-user-ID, through
# fakeroot(1). root has no password.
#
# With -t, the image is the boot test's: user "test" (uid 100, its
# password hashed with mkpasswd) and an /etc/rc.local that runs
# /usr/tests/boottest in the background as the system comes up.
#
# usage: mkroot.sh [-t] image sysdir mkpasswd
#

set -eu

test=0
if [ "${1:-}" = -t ]; then
        test=1
        shift
fi
if [ $# -ne 3 ]; then
        echo "usage: $0 [-t] image sysdir mkpasswd" >&2
        exit 1
fi
img=$1
sys=$2
mkpasswd=$3
MKE2FS=${MKE2FS:-/sbin/mke2fs}
FAKEROOT=${FAKEROOT:-fakeroot}
STRIP=${STRIP:-strip}

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT

mkdir -p "$src/bin" "$src/sbin" "$src/usr/bin" "$src/usr/libexec" \
    "$src/usr/tests" "$src/etc/defaults" "$src/dev" "$src/tmp" \
    "$src/root" "$src/home"

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

for f in rc rc.conf ttys passwd group shells defaults/rc.conf; do
        cp "etc/$f" "$src/etc/$f"
done

if [ $test -eq 1 ]; then
        install_prog boottest usr/tests/boottest
        hash=$("$mkpasswd" belka-test '$sha1$24680$belkatst$')
        echo "test:$hash:100:100:Boot test:/home/test:/bin/sh" \
            >> "$src/etc/passwd"
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
        chmod 0755 "$src"/bin/* "$src"/sbin/* "$src"/usr/libexec/*
        chmod 4555 "$src/usr/bin/login"
        chmod 1777 "$src/tmp"
        chmod 0700 "$src/root"
        if [ -d "$src/home/test" ]; then
                chown 100:100 "$src/home/test"
        fi
        "$2" -q -F -t ext2 -b 4096 -N 4096 -L belkaroot -d "$src" "$3" 64M
' sh "$src" "$MKE2FS" "$img"
