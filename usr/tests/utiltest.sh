#!/bin/sh
#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-14
# SPDX-License-Identifier: MIT
#
# The utilities (ksenon-utils) on Belka, run by usr/tests/boottest as
# root on the booted test image. It uses nothing but the utilities and
# the shell (mksh), and looks most at what rests on Belka's own calls:
# df (statvfs, /dev/mnttab), touch (utimensat), nice, tty (ttyname),
# chown, ln, and the device numbers ls shows; then the other programs:
# terminfo, ed, sed, grep, awk, ex and ps. The utilities' own suite runs
# on GNU/Linux.
#
# Prints "utiltest: N failures" and exits with N.

PATH=/bin:/sbin:/usr/bin:/usr/sbin
export PATH
LC_ALL=C
TZ=UTC0
export LC_ALL TZ

fail=0
pass=0

# check NAME EXPECTED COMMAND...: the command's output, with its last
# newline removed, must be EXPECTED, and its status 0.
check() {
        name=$1
        want=$2
        shift 2
        got=$("$@" 2>&1)
        st=$?
        if [ "$st" -eq 0 ] && [ "$got" = "$want" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                echo "utiltest: FAIL $name: status $st, want [$want], got [$got]"
        fi
}

# status NAME EXPECTED COMMAND...: only the exit status counts.
status() {
        name=$1
        want=$2
        shift 2
        diagnostics=$("$@" 2>&1)
        st=$?
        if [ "$st" -eq "$want" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                echo "utiltest: FAIL $name: status $st, want $want"
                printf '%s\n' "$diagnostics"
        fi
}

# The words of a command's output, one per line, for picking fields.
field() {
        n=$1
        shift
        set -- $("$@")
        eval "echo \${$n}"
}

work=/tmp/utiltest.$$
mkdir "$work" || { echo "utiltest: cannot make $work"; exit 1; }
cd "$work" || exit 1
umask 022

# Text.
check echo "a b" echo a b
check printf "x-5-0a" printf '%s-%d-%02x' x 5 10
check basename "c" basename /a/b/c.txt .txt
check dirname "/a/b" dirname /a/b/c
check expr "7" expr 3 + 4
status true 0 true
status false 1 false
printf 'pear\napple\npear\nfig\n' > fruit
check cat "pear
apple
pear
fig" cat fruit
check sort "apple
fig
pear
pear" sort fruit
check "sort | uniq -c" "1 apple
1 fig
2 pear" sh -c 'sort fruit | uniq -c'
check "wc -l" "4" sh -c 'wc -l < fruit'
check head "pear" head -n 1 fruit
check tail "fig" tail -n 1 fruit
check cut "ea" sh -c 'head -n 1 fruit | cut -c 2-3'
printf 'a\nb\n' > c1
printf 'b\nc\n' > c2
check comm "$(printf 'a\n\t\tb\n\tc')" comm c1 c2
check paste "$(printf 'a\tb\nb\tc')" paste c1 c2
check tr "PEAR" sh -c 'head -n 1 fruit | tr a-z A-Z'
check od " 61 62 0a" sh -c 'printf "ab\n" | od -An -tx1 | tr -s " "'
check dd "pe" sh -c 'dd if=fruit bs=1 count=2 2>/dev/null'
check tee "hi" sh -c 'echo hi | tee t1 >/dev/null && cat t1'
check env "FOO=bar" env -i FOO=bar /bin/env

# Files and directories.
check "mkdir -p, ls" "f" sh -c 'mkdir -p d/e && touch d/e/f && ls d/e'
check "cp, cmp" "" sh -c 'cp fruit f2 && cmp fruit f2'
check mv "f3" sh -c 'mv f2 f3 && ls f2 2>/dev/null; ls f3'
check "ln, links" "2" field 2 sh -c 'ln fruit hard && ls -l hard'
status "ln -s" 0 sh -c 'ln -s fruit soft && test -h soft'
check "chmod, ls -l" "-rw-r-----" field 1 sh -c 'chmod 640 fruit && ls -l fruit'
check "chown, ls -n" "100 100" sh -c 'chown 100:100 fruit && set -- $(ls -n fruit) && echo $3 $4'
check chgrp "0" sh -c 'chgrp 0 fruit && set -- $(ls -n fruit) && echo $4'
status mkfifo 0 sh -c 'mkfifo fifo && test -p fifo'
status du 0 du -s d
check "rm -r" "gone" sh -c 'rm -r d && test ! -d d && echo gone'
check rmdir "" sh -c 'mkdir x && rmdir x'
check pwd "$work" pwd

# Times: touch -t through utimensat, as ls -l shows them.
check "touch -t" "Jan 1 2010" sh -c 'touch -t 201001011200 t1 && set -- $(ls -l t1) && echo $6 $7 $8'
check "touch -r" "Jan 1 2010" sh -c 'touch -r t1 t2 && set -- $(ls -l t2) && echo $6 $7 $8'

# The mount table and statvfs.
check "mnttab root" "/dev/vd0 / ext2fs" sh -c 'set -- $(head -n 1 /etc/mnttab) && echo $1 $2 $3'
check "df -P /" "/" sh -c 'set -- $(df -P / | tail -n 1) && echo $6'
check "df -P /dev" "/dev" sh -c 'set -- $(df -P /dev | tail -n 1) && echo $6'
status "df blocks" 0 sh -c 'set -- $(df -P / | tail -n 1) && test "$2" -gt 0 && test "$4" -gt 0'

# mount and umount (mount(2), umount(2)): /etc/rc mounted /tmp from
# /etc/vfstab; the boot test's second disk, vd1, is blank ext2.
check "vfstab: /tmp" "swap /tmp tmpfs" awk '$2 == "/tmp" { print $1, $2, $3 }' /etc/mnttab
check "/tmp mode" "drwxrwxrwt" field 1 ls -ld /tmp
check "mount lists" "/ on /dev/vd0 read/write" sh -c 'mount | head -n 1 | { read m on s rw rest; echo $m $on $s $rw; }'
check "ls -l vd1" "b 6, 1" sh -c 'set -- $(ls -l /dev/vd1) && echo ${1%${1#?}} $5 $6'
status "mount vd1" 0 mount /dev/vd1 /mnt
check "mnttab vd1" "/dev/vd1 /mnt ext2fs rw" awk '$1 == "/dev/vd1" { print $1, $2, $3, $4 }' /etc/mnttab
check "df vd1" "/dev/vd1 /mnt" sh -c 'set -- $(df -P /mnt | tail -n 1) && echo $1 $6'
check "write on vd1" "on vd1" sh -c 'echo "on vd1" > /mnt/f && cat /mnt/f'
status "mount twice" 1 mount /dev/vd1 /mnt
status "mount twice elsewhere" 1 sh -c 'mkdir -p /tmp/m2 && mount /dev/vd1 /tmp/m2'
status "mount the root again" 1 sh -c 'mkdir -p /tmp/m3 && mount -r /dev/vd0 /tmp/m3'
status "umount busy" 1 sh -c 'cd /mnt && umount /mnt'
status "umount" 0 umount /mnt
check "unmounted" "" awk '$1 == "/dev/vd1"' /etc/mnttab
status "mount -r" 0 mount -r /dev/vd1 /mnt
check "read-only kept" "on vd1" cat /mnt/f
status "write refused" 1 sh -c 'echo x > /mnt/g'
check "mount says ro" "read-only" sh -c 'mount | grep "^/mnt " | { read m on s ro rest; echo $ro; }'
status "umount by device" 0 umount /dev/vd1
status "mount tmpfs" 0 mount -F tmpfs swap /mnt
check "move across" "f" sh -c 'echo z > /tmp/acrossf && mv /tmp/acrossf /mnt/f && ls /mnt && test ! -e /tmp/acrossf'
status "umount tmpfs" 0 umount /mnt
status "mount no type" 1 mount swap /mnt
status "mount not a disk" 1 mount -F ext2fs /dev/null /mnt

# su: root becomes anyone without a password.
check "su as root" "test" su test -c 'id -un'

# passwd's ageing and its state (passwd -s), on test2; then as it was.
check "passwd -s" "test2 PS" sh -c 'set -- $(passwd -s test2) && echo $1 $2'
status "passwd -x -n -w" 0 passwd -n 0 -x 30 -w 7 test2
check "passwd -s ageing" "0 30 7" sh -c 'set -- $(passwd -s test2) && echo $4 $5 $6'
check "shadow fields" "0:30:7" sh -c 'awk -F: "\$1 == \"test2\" { print \$4 \":\" \$5 \":\" \$6 }" /etc/shadow'
status "passwd back" 0 passwd -n -1 -x -1 -w -1 test2
check "passwd -s again" "3" sh -c 'set -- $(passwd -s test2) && echo $#'

# The host name: the boot set it from /etc/myname (rc.d/hostname), and
# uname, sysinfo and hostname agree; root may change it.
check "hostname" "belkatest" hostname
check "uname -n" "belkatest" uname -n
check "hostname -s" "belka" sh -c 'hostname belka.example.org && hostname -s'
check "uname -n, set" "belka.example.org" uname -n
status "hostname back" 0 hostname belkatest

# rcorder: PROVIDE, REQUIRE, BEFORE and KEYWORD, as /etc/rc uses them.
mkdir rco
printf '# PROVIDE: c\n# REQUIRE: b\n' > rco/c
printf '# PROVIDE: a\n# KEYWORD: nostart\n' > rco/a
printf '# PROVIDE: b\n# REQUIRE: a\n' > rco/b
printf '# PROVIDE: z\n# BEFORE: a\n\necho # PROVIDE: ignored\n' > rco/z
check "rcorder" "rco/z rco/a rco/b rco/c" sh -c 'echo $(rcorder rco/c rco/a rco/b rco/z)'
check "rcorder -s" "rco/z rco/b rco/c" sh -c 'echo $(rcorder -s nostart rco/c rco/a rco/b rco/z)'
check "rcorder -k" "rco/a" sh -c 'echo $(rcorder -k nostart rco/c rco/a rco/b rco/z)'
check "rcorder: none" "rco/n" sh -c 'printf "# REQUIRE: nothing\n" > rco/n && rcorder rco/n 2>/dev/null'
printf '# PROVIDE: x\n# REQUIRE: y\n' > rco/x
printf '# PROVIDE: y\n# REQUIRE: x\n' > rco/y
status "rcorder: circle" 1 rcorder rco/x rco/y
check "rc.d order" "mountall terminfo local" sh -c 'echo $(rcorder -s nostart /etc/rc.d/* | sed "s,.*/,," | grep -e mountall -e terminfo -e local)'

# Devices, the terminal and processes.
check "ls -l console" "c 1, 0" sh -c 'set -- $(ls -l /dev/console) && echo ${1%${1#?}} $5 $6'
check "ls -l null" "2, 0" sh -c 'set -- $(ls -l /dev/null) && echo $5 $6'
check "tty console" "/dev/console" sh -c 'tty </dev/console'
status "tty not" 1 sh -c 'tty </dev/null'
status "nice -n 5" 0 nice -n 5 true
status "nice -n -5, root" 0 nice -n -5 true
check "nice runs it" "ran" nice -n 19 sh -c 'echo ran'
check "id -u" "0" id -u
check "uname -s" "Belka" uname -s
# terminfo: the database /etc/rc compiled, and its programs.
status "terminfo.cdb" 0 test -s /usr/share/misc/terminfo.cdb
check "tput clear" " 033 [ H 033 [ 2 J" sh -c 'tput -T xterm clear | od -An -c | tr -s " "'
check "tput cols" "80" tput -T vt100 cols
check "tput colors" "256" tput -T xterm-256color colors
check "clear" " 033 [ H 033 [ 2 J" sh -c 'TERM=xterm clear | od -An -c | tr -s " "'
check "infocmp" "vt100|vt100-am|dec vt100 (w/advanced video)," sh -c 'infocmp vt100 | head -n 2 | tail -n 1'

# ed: write a file, change it, read it back.
check "ed" "hello
world" sh -c 'printf "a\nhello\nwurld\n.\n2s/u/o/\nw edfile\nq\n" | ed -s && cat edfile'
check "ed: line count" "12" sh -c 'printf "a\nhello\nworld\n.\nw edfile2\nq\n" | ed'

# sed, grep and awk (NetBSD's), over NetBSD's regex in the compat
# library.
printf 'alpha beta\nGamma delta\nepsilon\nbeta beta\n' > words
check "sed s///g" "alpha BETA
Gamma delta
epsilon
BETA BETA" sed 's/beta/BETA/g' words
check "sed -n p" "epsilon" sed -n '3p' words
check "sed -E backref" "beta alpha" sed -E -n '1s/^([a-z]+) ([a-z]+)$/\2 \1/p' words
check "sed y, d" "ALPHA BETA" sed -e '2,$d' -e 'y/abehlpt/ABEHLPT/' words
check "sed {}" "$(printf 'epsilon\nzeta')" sed -n -e '/^eps/{p' -e 's/.*/zeta/p' -e '}' words
check "sed -i" "one
TWO" sh -c 'printf "one\ntwo\n" > si && sed -i "s/two/TWO/" si && cat si'
check "sed -g \\|" "x" sh -c 'echo b | sed -g "s/a\\|b/x/"'
check grep "alpha beta
beta beta" grep beta words
check "grep -c -v" "2" grep -c -v beta words
check "grep -i" "Gamma delta" grep -i '^gamma' words
check "grep -w" "alpha beta" grep -w alpha words
check "grep -o" "beta
beta
beta" grep -o 'beta' words
check "grep -n" "3:epsilon" grep -n eps words
check "egrep" "Gamma delta
epsilon" egrep 'mm|^e' words
check "fgrep" "beta beta" fgrep 'ta b' words
check "grep -E -x" "epsilon" grep -E -x 'e[a-z]+' words
mkdir -p gr/sub && echo needle > gr/sub/hay && echo straw > gr/top
check "grep -r" "gr/sub/hay:needle" grep -r needle gr
check "grep -l" "words" grep -l Gamma words fruit
status "grep none" 1 grep zzz words
check "awk fields" "beta
delta" awk 'NR <= 2 { print $2 }' words
check "awk -F, NF" "3" sh -c 'echo a:b:c | awk -F: "{ print NF }"'
check "awk sum" "10" sh -c 'printf "1\n2\n3\n4\n" | awk "{ s += \$1 } END { print s }"'
check "awk arrays" "beta 3" awk '{ for (i = 1; i <= NF; i++) n[$i]++ } END { print "beta", n["beta"] }' words
check "awk printf, math" "2.72 1.00 8" awk 'BEGIN { printf "%.2f %.2f %d\n", exp(1), sin(3.14159265/2), 2^3 }'
check "awk gsub, length" "BxtB 4" sh -c 'echo beta | awk "{ gsub(/[ae]/, \"x\"); \$0 = toupper(substr(\$0, 1, 1)) substr(\$0, 2, 2) \"B\"; print \$0, length }"'
check "awk split, match" "3 b 2" awk 'BEGIN { n = split("a,b,c", p, ","); match("abc", /bc/); print n, p[2], RSTART }'
check "awk getline" "pear" awk 'BEGIN { getline l < "fruit"; print l }'
check "awk | sort" "apple
fig
pear" sh -c 'awk "!seen[\$0]++" fruit | sort'

# ex (nvi), in batch mode: its own regex, and its lines in db(3)'s
# recno. It wants a terminal type even so, and its standard error on a
# terminal, whose mode it sets for messages (mesg) as vi does.
check "ex -s" "h_ll_
w_rld" sh -c 'printf "a\nhello\nworld\n.\n%%s/[aeiou]/_/g\nw exfile\nq\n" | TERM=vt100 ex -s 2>/dev/console && cat exfile'
check "ex -s, read" "2" sh -c 'printf "\$=\nq\n" | TERM=vt100 ex -s exfile 2>/dev/console'

# ex locks the file it edits (flock, over fcntl's record locks): while
# one ex has it, a second finds it locked, edits it read-only, and its
# write fails; once the first has gone, the second may write.
check "ex: locked" "aaa" sh -c 'echo aaa > lockf; (sleep 3 | TERM=vt100 ex lockf >/dev/null 2>/dev/console) & sleep 1; printf "s/a/b/\nw\nq!\n" | TERM=vt100 ex -s lockf >/dev/null 2>/dev/console; wait; cat lockf'
check "ex: unlocked" "baa" sh -c 'printf "s/a/b/\nw\nq!\n" | TERM=vt100 ex -s lockf >/dev/null 2>/dev/console; cat lockf'

# The console's mode can change (login and vi's mesg change it).
check "chmod console" "crw-------" field 1 sh -c 'chmod 600 /dev/console && ls -l /dev/console'
status "chmod console back" 0 chmod 620 /dev/console

# ps, over /proc/pinfo.
check "ps -p 1" "1 init" sh -c 'set -- $(ps -p 1 | tail -n 1) && echo $1 $4'
check "ps -f -p 1" "root 0" sh -c 'set -- $(ps -f -p 1 | tail -n 1) && echo $1 $3'
check "ps -e: init" "yes" sh -c 'ps -e | while read pid tty time cmd; do [ "$cmd" = init ] && echo yes; done; true'
check "ps -l: state" "S" sh -c 'set -- $(ps -l -p $PPID | tail -n 1) && echo $2'
status date 0 date -u +%Y
status sleep 0 sleep 1
status who 0 who

# Archive round trips preserve names, bytes, symlinks, and hard links.
mkdir archive-src archive-out archive-pax archive-cpio
printf 'archive-data\n' > archive-src/file
ln archive-src/file archive-src/hard
ln -s file archive-src/link
mkfifo archive-src/fifo
status "tar create" 0 tar -cf test.tar archive-src
status "tar extract" 0 sh -c 'cd archive-out && tar -xf ../test.tar'
status "tar bytes" 0 cmp archive-src/file archive-out/archive-src/file
check "tar symlink" file readlink archive-out/archive-src/link
check "tar hard links" 2 stat -f %l archive-out/archive-src/file
check "tar fifo" prw-r--r-- stat -f %Sp archive-out/archive-src/fifo
status "pax create" 0 pax -w -x ustar -f test.pax archive-src
status "pax extract" 0 sh -c 'cd archive-pax && pax -r -f ../test.pax'
status "pax bytes" 0 cmp archive-src/file archive-pax/archive-src/file
status "pax copy" 0 pax -r -w archive-src archive-cpio
status "pax copy bytes" 0 cmp archive-src/file archive-cpio/archive-src/file
status "cpio create" 0 sh -c 'printf "archive-src/file\n" | cpio -o > test.cpio'
status "cpio extract" 0 sh -c 'cd archive-cpio && cpio -i < ../test.cpio'
status "archive truncated input" 1 sh -c 'printf bad | tar -tf - >/dev/null 2>&1'

# gzip streams, metadata, concatenated members, integrity errors, and tar -z.
printf 'compressed-data\n' > gzip-original
status "gzip stream" 0 sh -c 'gzip -n -c gzip-original > stream.gz'
status "gzip test" 0 gzip -t stream.gz
check "gunzip stream" compressed-data gunzip -c stream.gz
check "zcat alias" compressed-data zcat stream.gz
check "gzcat alias" compressed-data gzcat stream.gz
cat stream.gz stream.gz > joined.gz
check "gzip concatenated" "compressed-data
compressed-data" gunzip -c joined.gz
cp gzip-original gzip-file
chmod 640 gzip-file
touch -t 202001020304 gzip-file
original_time=$(stat -f %m gzip-file)
status "gzip file" 0 gzip gzip-file
status "gzip removes input" 1 test -e gzip-file
status "gunzip file" 0 gunzip gzip-file.gz
status "gzip round trip" 0 cmp gzip-original gzip-file
check "gzip permissions" 640 stat -f %Lp gzip-file
check "gzip modification time" "$original_time" stat -f %m gzip-file
printf 'preserved-output\n' > gzip-file.gz
status "gzip refuses overwrite" 1 gzip gzip-file
check "gzip overwrite content" preserved-output cat gzip-file.gz
status "gzip keeps source on failure" 0 cmp gzip-original gzip-file
printf '\037\213\010\000' > broken.gz
status "gzip truncated stream" 1 gzip -t broken.gz
mkdir gzip-tar-out
status "tar gzip create" 0 tar -czf test.tar.gz archive-src
status "tar gzip extract" 0 sh -c 'cd gzip-tar-out && tar -xzf ../test.tar.gz'
status "tar gzip bytes" 0 cmp archive-src/file gzip-tar-out/archive-src/file
status "mandoc gzip input" 0 sh -c 'gzip -n -c /usr/share/man/man1/stty.1 > stty.1.gz && mandoc -Tascii stty.1.gz > formatted'
status "mandoc gzip content" 0 grep -q STTY formatted

# NetBSD diff/patch produce and apply source changes in both directions.
printf 'one\ntwo\nthree\n' > diff-old
printf 'one\nTWO\nthree\nfour\n' > diff-new
status "diff same" 0 diff diff-old diff-old
status "diff differs" 1 sh -c 'diff -u diff-old diff-new > changes.diff'
status "diff missing input" 2 diff diff-missing diff-new
cp diff-old patch-target
status "patch unified" 0 patch patch-target changes.diff
status "patch bytes" 0 cmp patch-target diff-new
status "patch reverse" 0 patch -R patch-target changes.diff
status "patch reverse bytes" 0 cmp patch-target diff-old
status "patch dry run" 0 patch -C patch-target changes.diff
status "patch dry run bytes" 0 cmp patch-target diff-old
mkdir diff-tree-old diff-tree-new patch-tree
cp diff-old diff-tree-old/file
cp diff-new diff-tree-new/file
cp diff-old patch-tree/file
status "diff recursive" 1 sh -c 'diff -ru diff-tree-old diff-tree-new > tree.diff'
status "patch directory strip" 0 sh -c 'cd patch-tree && patch -p1 < ../tree.diff'
status "patch directory bytes" 0 cmp patch-tree/file diff-new
status "patch mismatch" 1 sh -c 'printf "unrelated\n" > mismatch && patch -f mismatch changes.diff'
check "patch mismatch preserved" unrelated cat mismatch
status "patch rejects" 0 test -s mismatch.rej

# Local account administration uses passwd/shadow/group as one transaction.
status "groupadd" 0 groupadd -g 2200 acct-primary
status "groupadd supplementary" 0 groupadd -g 2201 acct-extra
status "useradd" 0 useradd -m -u 2200 -g acct-primary -G acct-extra -s /bin/tcsh -c 'Account test' acct-test
check "useradd uid" 2200 id -u acct-test
check "useradd gid" 2200 id -g acct-test
status "useradd supplementary" 0 sh -c 'id -Gn acct-test | grep -q acct-extra'
check "useradd locked" LK sh -c 'set -- $(passwd -s acct-test); echo $2'
check "useradd home owner" 2200 stat -f %u /home/acct-test
check "useradd home group" 2200 stat -f %g /home/acct-test
status "useradd duplicate" 1 useradd acct-test
status "useradd duplicate uid" 1 useradd -u 2200 acct-duplicate
status "groupadd duplicate gid" 1 groupadd -g 2200 acct-duplicate
status "useradd invalid name" 1 useradd 'bad:name'
status "useradd invalid field" 1 useradd -c 'bad:comment' acct-invalid
status "useradd invalid shell" 1 useradd -s /bin/not-a-shell acct-invalid
status "useradd unknown group" 1 useradd -g acct-missing acct-invalid
status "useradd unknown supplementary group" 1 useradd -G acct-missing acct-invalid
status "failed useradd absent" 1 id acct-invalid
status "groupdel primary refused" 1 groupdel acct-primary
status "groupmod" 0 groupmod -g 2202 -n acct-primary-new acct-primary
check "groupmod primary migration" 2202 id -g acct-test
status "usermod" 0 usermod -l acct-renamed -u 2203 -G '' -s /bin/sh -c Updated acct-test
check "usermod uid" 2203 id -u acct-renamed
check "usermod comment" Updated awk -F: '$1 == "acct-renamed" { print $5 }' /etc/passwd
check "usermod shell" /bin/sh awk -F: '$1 == "acct-renamed" { print $7 }' /etc/passwd
check "usermod shadow name" LK sh -c 'set -- $(passwd -s acct-renamed); echo $2'
status "usermod clears supplementary" 1 sh -c 'id -Gn acct-renamed | grep -q acct-extra'
status "usermod old name absent" 1 id acct-test
status "usermod adds supplementary" 0 usermod -G acct-extra acct-renamed
status "usermod renames supplementary" 0 usermod -l acct-final acct-renamed
check "renamed supplementary member" acct-final awk -F: '$1 == "acct-extra" { print $4 }' /etc/group
status "userdel" 0 userdel acct-final
status "userdel passwd absent" 1 id acct-final
status "userdel shadow absent" 1 sh -c 'grep -q "^acct-final:" /etc/shadow'
check "userdel removes supplementary" "" awk -F: '$1 == "acct-extra" { print $4 }' /etc/group
status "groupdel primary" 0 groupdel acct-primary-new
status "groupdel supplementary" 0 groupdel acct-extra
status "account journal clean" 1 test -e /etc/.accttxn
status "home separately removable" 0 rmdir /home/acct-test

# Reconstruct an interrupted publication, then recover its original files.
cp /etc/passwd account-passwd-before
cp /etc/shadow account-shadow-before
cp /etc/group account-group-before
mkdir -m 700 /etc/.accttxn
ln /etc/passwd /etc/.accttxn/0.old
ln /etc/shadow /etc/.accttxn/1.old
ln /etc/group /etc/.accttxn/2.old
touch /etc/.accttxn/ready
sed 's/Superuser/Interrupted update/' /etc/passwd > /etc/acct-test-new
mv /etc/acct-test-new /etc/passwd
status "account rollback" 0 acctrecover
status "account passwd restored" 0 cmp account-passwd-before /etc/passwd
status "account shadow restored" 0 cmp account-shadow-before /etc/shadow
status "account group restored" 0 cmp account-group-before /etc/group
status "account rollback cleanup" 1 test -e /etc/.accttxn
check "shadow mode preserved" 400 stat -f %Lp /etc/shadow
status "account writers serialize" 0 sh -c 'groupadd acct-parallel-a & first=$!; groupadd acct-parallel-b & second=$!; wait "$first"; a=$?; wait "$second"; b=$?; [ "$a" -eq 0 ] && [ "$b" -eq 0 ]'
first_gid=$(awk -F: '$1 == "acct-parallel-a" { print $3 }' /etc/group)
second_gid=$(awk -F: '$1 == "acct-parallel-b" { print $3 }' /etc/group)
status "parallel unique groups" 0 test "$first_gid" != "$second_gid"
status "parallel first group" 0 groupdel acct-parallel-a
status "parallel second group" 0 groupdel acct-parallel-b

# A committed transaction keeps its published data during cleanup.
mkdir -m 700 /etc/.accttxn
ln /etc/passwd /etc/.accttxn/0.old
ln /etc/shadow /etc/.accttxn/1.old
ln /etc/group /etc/.accttxn/2.old
touch /etc/.accttxn/committed
sed 's/Superuser/Committed account update/' /etc/passwd > /etc/acct-test-new
mv /etc/acct-test-new /etc/passwd
status "account committed cleanup" 0 acctrecover
status "account committed data retained" 0 grep -q 'Committed account update' /etc/passwd
status "restore root comment" 0 usermod -c Superuser root
status "restored root record" 0 cmp account-passwd-before /etc/passwd
status "oversized uid refused" 1 useradd -u 4294967295 acct-invalid
# passwd-file writers also recover before making their own update.
mkdir -m 700 /etc/.accttxn
ln /etc/passwd /etc/.accttxn/0.old
ln /etc/shadow /etc/.accttxn/1.old
ln /etc/group /etc/.accttxn/2.old
touch /etc/.accttxn/ready
sed 's/Superuser/Interrupted writer update/' /etc/passwd > /etc/acct-test-new
mv /etc/acct-test-new /etc/passwd
status "chsh recovers account update" 0 chsh -s /bin/sh root
status "chsh recovered data" 0 cmp account-passwd-before /etc/passwd
status "chsh recovery cleanup" 1 test -e /etc/.accttxn



# Pager output is usable in pipelines, and mandoc indexes installed pages.
check "less pipe" pager-pipe sh -c 'printf "pager-pipe\n" | less'
check "more pipe" pager-pipe sh -c 'printf "pager-pipe\n" | more'
status "mandoc man macro" 0 sh -c 'mandoc -Tascii /usr/share/man/man1/less.1 > formatted'
status "mandoc output" 0 grep -q LESS formatted
status "mandoc mdoc macro" 0 sh -c 'mandoc -Tascii /usr/share/man/man1/stty.1 > formatted'
status "man lookup" 0 sh -c 'MANPAGER=cat man stty > formatted'
status "man content" 0 grep -q STTY formatted
mkdir -p manual-fixture/man1
cp /usr/share/man/man1/stty.1 manual-fixture/man1/
status "man fallback lookup" 0 sh -c 'MANPAGER=cat man -M "$PWD/manual-fixture" stty > formatted'
status "man fallback content" 0 grep -q STTY formatted
status "tcsh manual" 0 sh -c 'MANPAGER=cat man tcsh > formatted'
status "mandoc UTF-8" 0 sh -c 'mandoc -Tutf8 /usr/share/man/man1/stty.1 > formatted'
status "makewhatis" 0 makewhatis /usr/share/man
status "apropos lookup" 0 sh -c 'apropos stty | grep -q stty'
status "whatis lookup" 0 sh -c 'whatis chsh | grep -q chsh'
status "man missing" 5 sh -c 'man belka-no-such-manual >/dev/null 2>&1'

# Development tools: dependency scheduling, generators, archives, native ld.
cat > dev.mk <<'MAKE'
all: one two
	@cat one two > combined
one:
	@echo one > one
two:
	@echo two > two
MAKE
status "bmake parallel jobs" 0 bmake -j2 -f dev.mk
check "bmake dependencies" 'one
two' cat combined
rm one two combined
status "gmake parallel jobs" 0 gmake -j2 -f dev.mk
check "gmake dependencies" 'one
two' cat combined
status "gmake no-op" 0 gmake -q -f dev.mk one
check "tsort ordering" 'a
b
c' sh -c 'printf "a b\nb c\n" | tsort'
check "m4 expansion" hello sh -c 'printf "define(\140word\047,\140hello\047)word\n" | m4'
status "install directories" 0 install -d -m 700 dev-install
status "install file" 0 install -m 640 one dev-install/copy
status "install contents" 0 cmp one dev-install/copy
check "install mode" 640 stat -f %Lp dev-install/copy
cat > grammar.y <<'YACC'
%token NUMBER
%%
input: NUMBER;
%%
YACC
status "yacc generation" 0 yacc -d grammar.y
status "yacc header" 0 grep -q NUMBER y.tab.h
cat > scanner.l <<'LEX'
%option noyywrap
%%
[0-9]+ return 1;
. ;
%%
LEX
status "lex generation" 0 lex scanner.l
status "lex scanner" 0 grep -q yylex lex.yy.c
status "ar create" 0 ar rcs libfixture.a one two
check "ar contents" 'one
two' ar t libfixture.a
status "ar extract" 0 sh -c 'mkdir ar-out && cd ar-out && ar x ../libfixture.a'
status "ar extracted bytes" 0 cmp one ar-out/one
status "ranlib index" 0 ranlib libfixture.a
status "native archive" 0 ar rcs libnative.a /usr/tests/dev/devvalue.o
status "native archive index" 0 ranlib libnative.a
if command -v as >/dev/null 2>&1; then
        cat > native-main.s <<'ASM'
.text
.globl main
.type main,@function
main:
        xorl %eax, %eax
        ret
.section .note.GNU-stack,"",@progbits
ASM
        status "native assembly" 0 as --64 -o native-main.o native-main.s
        status "assembled program link" 0 sh -c 'ld -64 -dy -z defs -I /usr/lib64/ld.so.1 -o assembled /usr/lib64/crt1.o /usr/lib64/crti.o /usr/lib64/crtbegin.o native-main.o -L/usr/lib64 -lc /usr/lib64/libcompiler_rt.a /usr/lib64/crtend.o /usr/lib64/crtn.o'
        status "assembled program" 0 ./assembled
fi
status "native partial link" 0 ld -64 -r -o hello.o /usr/tests/dev/devhello.o
status "native executable link" 0 sh -c 'ld -64 -dy -z defs -I /usr/lib64/ld.so.1 -o hello /usr/lib64/crt1.o /usr/lib64/crti.o /usr/lib64/crtbegin.o hello.o libnative.a -L/usr/lib64 -lc /usr/lib64/libcompiler_rt.a /usr/lib64/crtend.o /usr/lib64/crtn.o'
check "native executable" 'native linker' ./hello
status "native static link" 0 sh -c 'ld -64 -dn -o hello.static /usr/lib64/crt1.o /usr/lib64/crti.o /usr/lib64/crtbegin.o hello.o libnative.a /usr/lib64/libc.a /usr/lib64/libcompiler_rt.a /usr/lib64/crtend.o /usr/lib64/crtn.o'
check "native static executable" 'native linker' ./hello.static
status "libelf m4 generation" 0 sh -c 'm4 < /usr/src/illumos-ld/usr/src/cmd/sgs/libelf/common/xlate.m4 > xlate.c'
status "libelf m4 compatibility" 0 cmp /usr/tests/dev/xlate.expected xlate.c
status "native message generator" 0 /usr/libexec/sgsmsg -i /usr/src/illumos-ld/usr/src/cmd/sgs/messages/sgs.ident -h native-msg.h -d native-msg.c /usr/src/illumos-ld/usr/src/cmd/sgs/ar/common/ar.msg
status "native message header" 0 grep -q MSG_ native-msg.h
status "nm native ld" 0 sh -c 'nm /usr/bin/ld > native-symbols'
status "nm symbols" 0 grep -q main native-symbols
status "native ld relink" 0 gmake -f /usr/src/illumos-ld/relink.mk
mv ld.stage2 ld.bootstrap
status "second stage relink" 0 gmake -f /usr/src/illumos-ld/relink.mk LD=./ld.bootstrap
status "linker reproducibility" 0 cmp ld.bootstrap ld.stage2

status "mkdir existing dot and root" 0 mkdir -p . ./ /

status "execvp scripts and PATH" 0 /usr/tests/exectest "$PWD"

. /usr/tests/buildtools.sh

if command -v gcc >/dev/null 2>&1; then
        . /usr/tests/compiler.sh
fi

cd /
[ "$fail" -ne 0 ] || rm -rf "$work"
echo "utiltest: $pass passed, $fail failures"
exit $fail
