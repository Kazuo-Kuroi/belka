#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-24
# SPDX-License-Identifier: MIT
#
# dig (and drill): DNS lookups in dig's manner, over djbdns' dnsqr, or
# dnsq for +norecurse. Not BIND's dig: the common forms only.
#
#   dig [@server] [-t type] [-x address] [name] [type] [+short]
#       [+norecurse]
#
# The server is the resolver /etc/resolv.conf names (dnscache) unless
# @server says otherwise; a name there is looked up first. The answer,
# authority and additional records are laid out as dig lays them out,
# names with their trailing dots; +short gives the answers' data alone.
# Record types djbdns does not decode come as their number and their
# bytes, escaped.

prog=${0##*/}
server=
type=
name=
short=0
recurse=1

usage()
{
        echo "usage: $prog [@server] [-t type] [-x address] [name] [type]" \
            "[+short] [+norecurse]" >&2
        exit 1
}

# a.b.c.d to d.c.b.a.in-addr.arpa
reverse()
{
        echo "$1" | awk -F. 'NF == 4 { print $4"."$3"."$2"."$1".in-addr.arpa" }'
}

is_type()
{
        case $(echo "$1" | tr '[:upper:]' '[:lower:]') in
        a|ns|mx|ptr|txt|cname|soa|hinfo|rp|sig|key|aaaa|axfr|any|srv|caa|\
        ds|dnskey|naptr|spf|[0-9]*)
                return 0 ;;
        esac
        return 1
}

while [ $# -gt 0 ]; do
        case $1 in
        @*)     server=${1#@} ;;
        -t)     [ $# -ge 2 ] || usage; type=$2; shift ;;
        -x)     [ $# -ge 2 ] || usage; name=$(reverse "$2"); type=ptr
                [ -n "$name" ] || usage; shift ;;
        +short) short=1 ;;
        +norec|+norecurse|+norecursive) recurse=0 ;;
        +rec|+recurse) recurse=1 ;;
        +*)     ;;              # dig's other options: passed over
        -h|--help) usage ;;
        -*)     usage ;;
        *)      if [ -z "$name" ]; then
                        name=$1
                elif [ -z "$type" ] && is_type "$1"; then
                        type=$1
                else
                        usage
                fi ;;
        esac
        shift
done
[ -n "$name" ] || name=.
[ -n "$type" ] || type=a
type=$(echo "$type" | tr '[:upper:]' '[:lower:]')

# The types dnsqr does not know by name, by number.
case $type in
srv)    qtype=33 ;;
caa)    qtype=257 ;;
ds)     qtype=43 ;;
dnskey) qtype=48 ;;
naptr)  qtype=35 ;;
spf)    qtype=99 ;;
*)      qtype=$type ;;
esac

# The server: an address, or a name looked up through dnscache.
if [ -n "$server" ]; then
        case $server in
        *[!0-9.]*)
                addr=$(dnsip "$server" | awk '{ print $1 }')
                if [ -z "$addr" ]; then
                        echo "$prog: couldn't get address for '$server'" >&2
                        exit 1
                fi ;;
        *)      addr=$server ;;
        esac
else
        addr=$(awk '$1 == "nameserver" { print $2; exit }' \
            /etc/resolv.conf 2>/dev/null)
        [ -n "$addr" ] || addr=127.0.0.1
fi

if [ $recurse -eq 1 ]; then
        out=$(DNSCACHEIP=$addr dnsqr "$qtype" "$name" 2>&1)
else
        out=$(dnsq "$qtype" "$name" "$addr" 2>&1)
fi
status=$?
if [ $status -ne 0 ]; then
        case $out in
        *timed\ out*|*unable\ to\ find*|*connection\ refused*)
                echo ";; connection timed out; no servers could be reached"
                ;;
        *)      echo ";; $(echo "$out" | sed -n '$p')" ;;
        esac
        exit 9
fi

echo "$out" | awk -v short=$short -v qname="$name" -v qtype="$type" \
    -v server="$addr" -v prog="$prog" '
function dot(n) { return n ~ /\.$/ ? n : n "." }
# A record as dig shows it, from djbdns: "name ttl TYPE data".
function record(s,    f, n, i, data, t) {
        n = split(s, f, " ")
        t = f[3]
        data = ""
        if (t == "NS" || t == "CNAME" || t == "PTR")
                data = dot(f[4])
        else if (t == "MX")
                data = f[4] " " dot(f[5])
        else if (t == "SOA")
                data = dot(f[4]) " " dot(f[5]) " " f[6] " " f[7] " " f[8] \
                    " " f[9] " " f[10]
        else if (t == "A")
                data = f[4]
        else {
                for (i = 4; i <= n; i++)
                        data = data (i > 4 ? " " : "") f[i]
                if (t == "16") {
                        t = "TXT"
                        data = txt(data)
                } else if (t == "28") {
                        t = "AAAA"
                        data = aaaa(data)
                } else {
                        t = "TYPE" t
                }
        }
        if (short)
                return data
        return dot(f[1]) "\t" f[2] "\tIN\t" t "\t" data
}
# djbdns escapes bytes as \ooo; the octets, in order.
function octets(s, b,    n, i, c) {
        n = 0
        for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c == "\\" && substr(s, i + 1, 3) ~ /^[0-7][0-7][0-7]$/) {
                        b[++n] = (substr(s, i + 1, 1) * 64) + \
                            (substr(s, i + 2, 1) * 8) + substr(s, i + 3, 1)
                        i += 3
                } else {
                        b[++n] = ord[c]
                }
        }
        return n
}
# TXT: its strings, each a length and its bytes.
function txt(s,    b, n, i, k, out, c) {
        n = octets(s, b)
        out = ""
        for (i = 1; i <= n; i += k + 1) {
                k = b[i]
                out = out (out != "" ? " " : "") "\""
                for (c = i + 1; c <= i + k && c <= n; c++)
                        out = out (b[c] == 34 ? "\\\"" : sprintf("%c", b[c]))
                out = out "\""
        }
        return out
}
# AAAA: sixteen bytes, as eight groups of hex (not shortened).
function aaaa(s,    b, n, i, out) {
        n = octets(s, b)
        out = ""
        for (i = 1; i + 1 <= n; i += 2)
                out = out (i > 1 ? ":" : "") sprintf("%x", b[i] * 256 + b[i + 1])
        return out
}
BEGIN {
        for (i = 32; i < 127; i++)
                ord[sprintf("%c", i)] = i
        sec = ""
}
NR == 2 {
        status = "NOERROR"
        if ($0 ~ /nxdomain/) status = "NXDOMAIN"
        if ($0 ~ /servfail/) status = "SERVFAIL"
        if ($0 ~ /refused/) status = "REFUSED"
        if ($0 ~ /notimp/) status = "NOTIMP"
        if ($0 ~ /formerr/) status = "FORMERR"
        if ($0 ~ /weird rcode/) status = "UNKNOWN"
        flags = ($0 ~ /authoritative/ ? " aa" : "") \
            ($0 ~ /truncated/ ? " tc" : "")
        next
}
/^answer: / {
        r = record(substr($0, 9))
        if (short)
                print r
        else
                ans[++nans] = r
        next
}
/^authority: / { auth[++nauth] = record(substr($0, 12)); next }
/^additional: / { add[++nadd] = record(substr($0, 13)); next }
END {
        if (short)
                exit
        printf("; <<>> %s (Belka, over djbdns) <<>> %s %s\n", prog,
            qname, toupper(qtype))
        printf(";; ->>HEADER<<- status: %s\n", status)
        printf(";; flags:%s; ANSWER: %d, AUTHORITY: %d, ADDITIONAL: %d\n\n",
            flags, nans, nauth, nadd)
        printf(";; QUESTION SECTION:\n;%s\t\t\tIN\t%s\n", dot(qname),
            toupper(qtype))
        if (nans) {
                print "\n;; ANSWER SECTION:"
                for (i = 1; i <= nans; i++) print ans[i]
        }
        if (nauth) {
                print "\n;; AUTHORITY SECTION:"
                for (i = 1; i <= nauth; i++) print auth[i]
        }
        if (nadd) {
                print "\n;; ADDITIONAL SECTION:"
                for (i = 1; i <= nadd; i++) print add[i]
        }
        printf("\n;; SERVER: %s#53\n\n", server)
}'
