#!/bin/sh
# usage: tracker_daemon_test.sh mbt-tracker
# Runs the daemon on real sockets. On OpenBSD this also runs its pledge.
set -u
TRACKER=$1
command -v nc >/dev/null || exit 77
PORT=$((20000 + $$ % 20000))
OUT=$(mktemp)
H=aaaaaaaaaaaaaaaaaaaa

"$TRACKER" -u "$PORT" -H "$PORT" 2>/dev/null &
PID=$!
trap 'kill $PID 2>/dev/null; rm -f "$OUT"' EXIT
sleep 0.3

fail() {
	echo "tracker_daemon: $1" >&2
	exit 1
}

http() {
	printf 'GET %s HTTP/1.0\r\nHost: localhost\r\n\r\n' "$1" |
		nc -w 3 127.0.0.1 "$PORT"
}

Q="info_hash=$H&peer_id=-MB0001-000000000000&port=6881"
Q="$Q&uploaded=0&downloaded=0&left=0&event=started"
case $(http "/announce?$Q") in
*8:interval*) ;;
*) fail "no announce reply" ;;
esac
case $(http "/scrape?info_hash=$H") in
*8:complete*) ;;
*) fail "no scrape reply" ;;
esac

# BEP 15 connect: magic, action 0, transaction id.
printf '\000\000\004\027\047\020\031\200\000\000\000\000abcd' |
	nc -u -w 1 127.0.0.1 "$PORT" > "$OUT"
[ "$(wc -c < "$OUT")" -eq 16 ] || fail "no UDP reply"

kill -0 $PID 2>/dev/null || fail "daemon died"
echo "tracker_daemon: ok"
