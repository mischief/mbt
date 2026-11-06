#!/bin/sh
# Run AFL++ with ASan and UBSan on one harness: bencode (default), metainfo, torrent, dht, lsd or magnet.
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

# The PCGUARD pass crashes clang 21 on UBSan code, and a crash starts a large
# llvm-symbolizer for each job. NATIVE coverage works with both sanitizers.
export AFL_USE_ASAN=1 AFL_USE_UBSAN=1 AFL_LLVM_INSTRUMENT=NATIVE
export LLVM_DISABLE_SYMBOLIZATION=1
# Fill all of each allocation, so that a test cannot read what an earlier one left.
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:symbolize=0:detect_stack_use_after_return=1:max_malloc_fill_size=1073741824"
export UBSAN_OPTIONS="abort_on_error=1:halt_on_error=1:symbolize=0"

if [ ! -d build-fuzz ]; then
	CC=afl-clang-fast meson setup build-fuzz \
	    -Ddefault_library=static -Dwerror=false
fi
ninja -C build-fuzz "fuzz/fuzz_$name"

dict=
[ -f "fuzz/$name.dict" ] && dict="-x fuzz/$name.dict"

exec env \
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
    AFL_SKIP_CPUFREQ=1 \
    afl-fuzz -i "$corpus" -o "build-fuzz/fuzz-out-$name" \
    $dict ${AFL_FUZZ_ARGS:-} \
    -- "./build-fuzz/fuzz/fuzz_$name"
