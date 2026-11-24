#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-24
# SPDX-License-Identifier: MIT
#
# host: DNS lookups in host's manner, over djbdns' dnsqr. Not BIND's
# host: the common forms only.
#
#   host [-t type] name|address [server]
#
# An address is looked up by its PTR record; a name by its A and MX
# records (and an alias it is, if any), or by -t's type. The server is
# the resolver /etc/resolv.conf names (dnscache) unless one is given.
# Exits 1 if the name does not exist, or nothing could be asked.

prog=${0##*/}
type=

usage()
{
        echo "usage: $prog [-t type] name|address [server]" >&2
        exit 1
}

while getopts at:v c; do
        case $c in
        a)      type=any ;;
        t)      type=$(echo "$OPTARG" | tr '[:upper:]' '[:lower:]') ;;
        v)      ;;
        *)      usage ;;
        esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] && [ $# -le 2 ] || usage
name=$1
server=${2:-}

if [ -n "$server" ]; then
        case $server in
        *[!0-9.]*) addr=$(dnsip "$server" | awk '{ print $1 }') ;;
        *)      addr=$server ;;
        esac
        if [ -z "$addr" ]; then
                echo "$prog: couldn't get address for '$server'" >&2
                exit 1
        fi
        export DNSCACHEIP=$addr
        echo "Using domain server:"
        echo "Name: $server"
        echo "Address: $addr#53"
        echo
fi

# An address: its PTR, under in-addr.arpa.
case $name in
*[!0-9.]*) ;;
*.*.*.*)
        rev=$(echo "$name" | awk -F. '{ print $4"."$3"."$2"."$1".in-addr.arpa" }')
        [ -n "$type" ] || type=ptr
        name=$rev ;;
esac

# One query, its answers as host shows them; $1 the type, $2 the name.
ask()
{
        out=$(dnsqr "$1" "$2" 2>&1) || {
                echo ";; connection timed out; no servers could be reached"
                exit 1
        }
        echo "$out" | awk -v q="$2" '
        function dot(n) { return n ~ /\.$/ ? n : n "." }
        NR == 2 {
                if ($0 ~ /nxdomain/) { nx = 1 }
                if ($0 ~ /servfail/) { sf = 1 }
                next
        }
        /^answer: / {
                n = split(substr($0, 9), f, " ")
                if (f[3] == "A")
                        print f[1] " has address " f[4]
                else if (f[3] == "CNAME")
                        print f[1] " is an alias for " dot(f[4])
                else if (f[3] == "MX")
                        print f[1] " mail is handled by " f[4] " " dot(f[5])
                else if (f[3] == "NS")
                        print f[1] " name server " dot(f[4])
                else if (f[3] == "PTR")
                        print f[1] " domain name pointer " dot(f[4])
                else if (f[3] == "SOA")
                        print f[1] " has SOA record " dot(f[4]) " " dot(f[5]) \
                            " " f[6] " " f[7] " " f[8] " " f[9] " " f[10]
                else {
                        data = ""
                        for (i = 4; i <= n; i++)
                                data = data (i > 4 ? " " : "") f[i]
                        print f[1] " has type" f[3] " record " data
                }
                any = 1
                next
        }
        END {
                if (nx)
                        exit 3
                if (sf)
                        exit 2
                exit any ? 0 : 4
        }'
}

if [ -n "$type" ]; then
        ask "$type" "$name"
        st=$?
else
        ask a "$name"
        st=$?
        [ $st -eq 3 ] || { ask mx "$name"; [ $? -eq 0 ] && st=0; }
fi
case $st in
0)      exit 0 ;;
3)      echo "Host $name not found: 3(NXDOMAIN)"; exit 1 ;;
2)      echo "Host $name not found: 2(SERVFAIL)"; exit 1 ;;
*)      [ -n "$type" ] && echo "$name has no $(echo "$type" |
            tr '[:lower:]' '[:upper:]') record"; exit 0 ;;
esac
