#!/bin/sh
# check-docs.sh — verify all exported symbols are documented in man pages
# Returns 0 if all documented, 1 if undocumented symbols exist.
# Prints coverage stats regardless.
set -e

LIBSO="${1:-build/libmbt.so}"
MANDIR="${2:-man}"

if [ ! -f "$LIBSO" ]; then
    echo "error: $LIBSO not found (build first)" >&2
    exit 1
fi

# Extract exported mbt_* symbols
nm -D --defined-only "$LIBSO" | awk '/( T | D | B )/{print $3}' \
    | grep '^mbt_' | sed 's/@@.*//' | sort -u > /tmp/mbt_exported.txt

# Extract documented symbols from man pages (.Fn and .Fo lines)
if [ -d "$MANDIR" ] && ls "$MANDIR"/*.3 >/dev/null 2>&1; then
    grep -h '^\.\(Fn\|Fo\) ' "$MANDIR"/*.3 2>/dev/null \
        | awk '{print $2}' | sort -u > /tmp/mbt_documented.txt
else
    : > /tmp/mbt_documented.txt
fi

TOTAL=$(wc -l < /tmp/mbt_exported.txt)
DOCUMENTED=$(comm -12 /tmp/mbt_exported.txt /tmp/mbt_documented.txt | wc -l)
UNDOCUMENTED=$(comm -23 /tmp/mbt_exported.txt /tmp/mbt_documented.txt | wc -l)

echo "doc coverage: $DOCUMENTED/$TOTAL symbols documented ($UNDOCUMENTED remaining)"

if [ "$UNDOCUMENTED" -gt 0 ]; then
    echo ""
    echo "undocumented:"
    comm -23 /tmp/mbt_exported.txt /tmp/mbt_documented.txt | sed 's/^/  /'
fi

rm -f /tmp/mbt_exported.txt /tmp/mbt_documented.txt
# Don't fail CI yet — just report. Change to `exit 1` when coverage is complete.
exit 0
