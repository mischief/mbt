#!/bin/sh
# test.sh — run full test suite (unit, ASan, valgrind, symbol check)
# Requires: meson, ninja, clang, valgrind, sd-event or libevent
set -e

BACKEND=${1:-sd-event}
echo "=== libmbt test suite (backend: $BACKEND) ==="

# 1. Standard build + tests
echo "--- build + meson test ---"
rm -rf build
meson setup build -Devent_backend=$BACKEND
ninja -C build
meson test -C build

# 2. Symbol visibility check
echo "--- symbol visibility ---"
LEAKED=$(nm -D build/libmbt.so | grep " T " | awk '{print $3}' | grep -v "^mbt_" || true)
if [ -n "$LEAKED" ]; then
    echo "FAIL: un-namespaced symbols exported:"
    echo "$LEAKED"
    exit 1
fi
echo "OK: all exported symbols are mbt_*"

# 3. Clang + ASan (static lib)
echo "--- clang + ASan ---"
rm -rf build
CC=clang meson setup build -Devent_backend=$BACKEND -Db_sanitize=address -Ddefault_library=static
ninja -C build
meson test -C build

# 4. Valgrind
echo "--- valgrind ---"
rm -rf build
meson setup build -Devent_backend=$BACKEND
ninja -C build
meson test -C build --wrapper='valgrind --error-exitcode=1 --leak-check=full --quiet'

echo "=== ALL PASSED ==="
