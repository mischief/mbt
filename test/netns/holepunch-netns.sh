#!/bin/sh
# SPDX-License-Identifier: ISC
#
# BEP 55 holepunch over real Linux NATs, built from network namespaces.
# Run as root on a Linux box with nftables NAT support (a throwaway VM).
#
#   ns hp-r   10.0.0.10                relay R: seeder, public, reachable
#   ns hp-na  10.0.0.2 | 192.168.1.1   NAT for A
#   ns hp-nc  10.0.0.3 | 192.168.2.1   NAT for C
#   ns hp-a   192.168.1.2              A: leecher behind NAT A
#   ns hp-c   192.168.2.2              C: seeder behind NAT C
#   ns hp-net bridge joining R and both NAT WAN sides ("the internet")
#
# Each NAT masquerades its LAN and drops unsolicited inbound packets (only
# conntrack replies pass): endpoint-dependent filtering. MODE=easy keeps
# source ports where it can (the Linux default, endpoint-independent
# mapping); MODE=random adds "random" so every new flow gets a random port,
# like pf without static-port (symmetric NAT).
#
# C seeds, so it never dials out; A learns C from R's PEX and dials it. The
# direct SYN dies at NAT C, A asks R for a rendezvous, and with MODE=easy
# the two connect. Pass/fail is decided from the logs.
#
# usage: holepunch-netns.sh <builddir> [easy|random]
set -eu

B=$(cd "${1:?builddir}" && pwd)
MODE=${2:-easy}
GET=$B/mbt-get
TOR=$B/mbt-torrent
W=$(mktemp -d /tmp/hp.XXXXXX)
NS="hp-net hp-r hp-na hp-nc hp-a hp-c"

cleanup() {
	for n in $NS; do
		ip netns pids "$n" 2>/dev/null | xargs -r kill 2>/dev/null || true
	done
	sleep 1
	for n in $NS; do ip netns del "$n" 2>/dev/null || true; done
}
trap cleanup EXIT INT TERM
cleanup

for n in $NS; do ip netns add "$n"; ip -n "$n" link set lo up; done

# the internet: a bridge in hp-net
ip -n hp-net link add br0 type bridge
ip -n hp-net link set br0 up
wan() { # wan <ns> <addr>
	ip link add "w-$1" type veth peer name wan netns "$1"
	ip link set "w-$1" netns hp-net
	ip -n hp-net link set "w-$1" master br0 up
	ip -n "$1" addr add "$2/24" dev wan
	ip -n "$1" link set wan up
}
wan hp-r  10.0.0.10
wan hp-na 10.0.0.2
wan hp-nc 10.0.0.3

lan() { # lan <host ns> <nat ns> <net>
	ip link add lan netns "$2" type veth peer name eth0 netns "$1"
	ip -n "$2" addr add "$3.1/24" dev lan
	ip -n "$2" link set lan up
	ip -n "$1" addr add "$3.2/24" dev eth0
	ip -n "$1" link set eth0 up
	ip -n "$1" route add default via "$3.1"
	ip netns exec "$2" sysctl -qw net.ipv4.ip_forward=1
	if [ "$MODE" = random ]; then masq="masquerade random"; else masq="masquerade"; fi
	ip netns exec "$2" nft -f - <<EOF
table ip nat {
	chain postnat { type nat hook postrouting priority 100; oifname "wan" $masq; }
}
table ip filter {
	chain forward_lan {
		type filter hook forward priority 0; policy drop;
		ct state established,related accept
		iifname "lan" oifname "wan" accept
		iifname "wan" counter drop
	}
	# Unsolicited packets to the NAT's own address must be dropped in the
	# filter, as a real router does: conntrack only confirms an entry for
	# a packet that passes, so a dropped one leaves no state. If they were
	# accepted (no listener, but still accepted), each would create a
	# conntrack entry whose reply tuple then clashes with the host's own
	# outgoing mapping for the same flow, and masquerade would move the
	# host to another port -- defeating the simultaneous open.
	chain input_wan {
		type filter hook input priority 0; policy accept;
		iifname "wan" ct state established,related accept
		iifname "wan" icmp type echo-request accept
		iifname "wan" counter drop
	}
}
EOF
}
lan hp-a hp-na 192.168.1
lan hp-c hp-nc 192.168.2

# sanity: A reaches R
ip netns exec hp-a ping -c1 -W2 10.0.0.10 >/dev/null

# payload: 16 MiB so A, rate limited, is still leeching when PEX arrives
mkdir -p "$W/r" "$W/c" "$W/a"
head -c $((16 * 1024 * 1024)) /dev/urandom > "$W/r/hp.bin"
cp "$W/r/hp.bin" "$W/c/hp.bin"
"$TOR" create -o "$W/hp.torrent" "$W/r/hp.bin" >/dev/null
IH=$("$TOR" show "$W/hp.torrent" | awk '/^Hash:/ {print $2; exit}')
[ -n "$IH" ] || { echo "no infohash"; exit 1; }

common="-d -q -L -T"
ip netns exec hp-r "$GET" $common -S -l 7000 -o "$W/r" "$W/hp.torrent" > "$W/r.log" 2>&1 &
sleep 2
ip netns exec hp-c "$GET" $common -S -l 7001 -o "$W/c" \
	"magnet:?xt=urn:btih:$IH&x.pe=10.0.0.10:7000" > "$W/c.log" 2>&1 &
sleep 3
ip netns exec hp-a "$GET" $common -r 64 -l 7002 -o "$W/a" \
	"magnet:?xt=urn:btih:$IH&x.pe=10.0.0.10:7000" > "$W/a.log" 2>&1 &

# R's PEX (60 s interval) tells A about C; give the rest time to happen
end=$(( $(date +%s) + ${HP_WAIT:-150} ))
while [ "$(date +%s)" -lt "$end" ]; do
	if grep -q "holepunch uTP connect to .* failed" "$W/a.log" ||
	   grep -q "uTP connected to 10.0.0.3:" "$W/a.log" ||
	   grep -q "uTP connected to 10.0.0.2:" "$W/c.log"; then
		sleep 5
		break
	fi
	sleep 2
done

echo "== mode $MODE, logs in $W"
echo "-- NAT C inbound drops"; ip netns exec hp-nc nft list table ip filter | grep counter
echo "-- NAT A inbound drops"; ip netns exec hp-na nft list table ip filter | grep counter
echo "-- R"; grep -E "holepunch" "$W/r.log" | head
echo "-- A"; grep -E "holepunch|10\.0\.0\.3|duplicate" "$W/a.log" | head -20
echo "-- C"; grep -E "holepunch|10\.0\.0\.2|duplicate" "$W/c.log" | head -20

ok=0
grep -q "holepunch: relaying" "$W/r.log" || { echo "FAIL: R never relayed"; ok=1; }
if [ "$MODE" = easy ]; then
	grep -q "uTP connected to 10.0.0.3:" "$W/a.log" ||
	grep -q "uTP connected to 10.0.0.2:" "$W/c.log" ||
		{ echo "FAIL: A and C did not connect"; ok=1; }
else
	grep -q "holepunch uTP connect to .* failed" "$W/a.log" ||
		{ echo "FAIL: expected the holepunch to fail cleanly"; ok=1; }
fi
[ $ok = 0 ] && echo "PASS ($MODE)"
exit $ok
