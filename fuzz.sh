#!/bin/sh
# Run AFL++ with ASan on one harness: bencode (default), metainfo, torrent, dht, lsd or magnet.
set -e

cd "$(dirname "$0")"

name=${1:-bencode}
case "$name" in
bencode)
	corpus=fuzz/corpus
	;;
metainfo|torrent|dht|lsd|magnet)
	corpus=fuzz/corpus_$name
	;;
*)
	echo "usage: $0 [bencode|metainfo|torrent|dht|lsd|magnet]" >&2
	exit 1
	;;
esac

if [ ! -d build-fuzz ]; then
	CC=afl-clang-fast AFL_USE_ASAN=1 meson setup build-fuzz \
	    -Ddefault_library=static -Dwerror=false
fi
AFL_USE_ASAN=1 ninja -C build-fuzz "fuzz/fuzz_$name"

exec env \
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
    AFL_SKIP_CPUFREQ=1 \
    afl-fuzz -i "$corpus" -o "build-fuzz/fuzz-out-$name" \
    ${AFL_FUZZ_ARGS:-} \
    -- "./build-fuzz/fuzz/fuzz_$name"
