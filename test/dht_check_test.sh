#!/bin/sh
# usage: dht_check_test.sh mbt-dhtd mbt-dht-check
# A private node with a dead bootstrap address, so no public traffic.
# The name makes the resolver run under the sandbox of the node.
set -u
DHTD=$1
CHECK=$2
PORT=$((20000 + $$ % 20000))
A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
C=cccccccccccccccccccccccccccccccccccccccc
LIST=$(mktemp)

"$DHTD" -p "$PORT" -b localhost:1 2>/dev/null &
NODE=$!
trap 'kill $NODE 2>/dev/null; rm -f "$LIST"' EXIT
sleep 0.3

fail() {
	echo "dht_check: $1" >&2
	exit 1
}

"$CHECK" "127.0.0.1:$PORT" "$A" >/dev/null && fail "unknown hash passed"
OUT=$("$CHECK" -a 7777 "127.0.0.1:$PORT" "$A" "$B") || fail "announce failed: $OUT"
case $OUT in
"checked 2: peers 2, no peers 0, no reply 0, announced 2, node stores 2") ;;
*) fail "announce summary: $OUT" ;;
esac

printf '# list\n%s\n\n%s\n%s\n' "$A" "$B" "$C" > "$LIST"
OUT=$("$CHECK" -v -f "$LIST" "127.0.0.1:$PORT") && fail "partial list passed"
case $OUT in
*"$A peers 1 nodes"*) ;;
*) fail "per hash line: $OUT" ;;
esac
case $OUT in
*"checked 3: peers 2, no peers 1, no reply 0, node stores 2") ;;
*) fail "list summary: $OUT" ;;
esac

"$CHECK" "127.0.0.1:$PORT" nothex 2>/dev/null
[ $? -eq 2 ] || fail "bad hash status"
kill $NODE
OUT=$(timeout 20 "$CHECK" "127.0.0.1:$PORT" "$A") && fail "dead node passed"
case $OUT in
"checked 1: peers 0, no peers 0, no reply 1") ;;
*) fail "dead node summary: $OUT" ;;
esac
echo "dht_check: ok"
