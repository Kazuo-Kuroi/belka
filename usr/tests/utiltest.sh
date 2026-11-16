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

PATH=/bin:/sbin:/usr/bin
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
        "$@" >/dev/null 2>&1
        st=$?
        if [ "$st" -eq "$want" ]; then
                pass=$((pass + 1))
        else
                fail=$((fail + 1))
                echo "utiltest: FAIL $name: status $st, want $want"
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
check "mnttab root" "vd0 / ext2fs" sh -c 'set -- $(head -n 1 /etc/mnttab) && echo $1 $2 $3'
check "df -P /" "/" sh -c 'set -- $(df -P / | tail -n 1) && echo $6'
check "df -P /dev" "/dev" sh -c 'set -- $(df -P /dev | tail -n 1) && echo $6'
status "df blocks" 0 sh -c 'set -- $(df -P / | tail -n 1) && test "$2" -gt 0 && test "$4" -gt 0'

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

cd /
rm -rf "$work"
echo "utiltest: $pass passed, $fail failures"
exit $fail
