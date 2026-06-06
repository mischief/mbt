#!/bin/sh
# Run AFL++ fuzzer on bencode parser
set -e

cd "$(dirname "$0")"

CC=afl-clang-fast meson setup build-fuzz --wipe >/dev/null 2>&1
ninja -C build-fuzz be_fuzz >/dev/null 2>&1

exec env \
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
    AFL_SKIP_CPUFREQ=1 \
    afl-fuzz -i fuzz/corpus -o build-fuzz/fuzz-out \
    ${AFL_FUZZ_ARGS:-} \
    -- ./build-fuzz/be_fuzz
