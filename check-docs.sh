#!/bin/sh
# check-docs.sh — verify all exported symbols are documented in man pages
# Also validates MBT_API annotation coverage and lints man pages with mandoc.
# Exits non-zero if any symbols are undocumented or mandoc reports errors.
set -e

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT

LIBSO="${1:-build/libmbt.so}"
MANDIR="${2:-man}"

if [ ! -f "$LIBSO" ]; then
    echo "error: $LIBSO not found (build first)" >&2
    exit 1
fi

# Extract exported mbt_* symbols
nm -D --defined-only "$LIBSO" | awk '/( T | D | B )/{print $3}' \
    | grep '^mbt_' | sed 's/@@.*//' | sort -u > "$TMPD/exported.txt"

# Extract documented symbols from man pages (.Fn and .Fo lines)
if [ -d "$MANDIR" ] && ls "$MANDIR"/*.3 >/dev/null 2>&1; then
    grep -h '\.\(Fn\|Fo\) ' "$MANDIR"/*.3 2>/dev/null \
        | awk '{print $2}' | sort -u > "$TMPD/documented.txt"
else
    : > "$TMPD/documented.txt"
fi

TOTAL=$(wc -l < "$TMPD/exported.txt")
DOCUMENTED=$(comm -12 "$TMPD/exported.txt" "$TMPD/documented.txt" | wc -l)
UNDOCUMENTED=$(comm -23 "$TMPD/exported.txt" "$TMPD/documented.txt" | wc -l)

echo "doc coverage: $DOCUMENTED/$TOTAL symbols documented ($UNDOCUMENTED remaining)"

# Validate MBT_API: check for public function decls missing MBT_API
MISSING_API=$(grep -rhn '^[a-z].*mbt_.*(' mbt/*.h 2>/dev/null \
    | grep -v 'MBT_API\|typedef\|#define\|static\|export.h' || true)
if [ -n "$MISSING_API" ]; then
    echo ""
    echo "WARNING: function declarations missing MBT_API:"
    echo "$MISSING_API"
fi

if [ "$UNDOCUMENTED" -gt 0 ]; then
    echo ""
    echo "undocumented:"
    comm -23 "$TMPD/exported.txt" "$TMPD/documented.txt" | sed 's/^/  /'
fi

# Lint man pages with mandoc if available
LINT_ERRORS=0
if command -v mandoc >/dev/null 2>&1 && [ -d "$MANDIR" ] && ls "$MANDIR"/*.3 >/dev/null 2>&1; then
    for f in "$MANDIR"/*.3; do
        errs=$(mandoc -Tlint "$f" 2>&1 | grep "ERROR" | wc -l)
        if [ "$errs" -gt 0 ]; then
            echo ""
            echo "mandoc errors in $f:"
            mandoc -Tlint "$f" 2>&1 | grep "ERROR" | sed 's/^/  /'
            LINT_ERRORS=$((LINT_ERRORS + errs))
        fi
    done
    if [ "$LINT_ERRORS" -gt 0 ]; then
        echo ""
        echo "total mandoc errors: $LINT_ERRORS"
    fi
fi

[ "$UNDOCUMENTED" -eq 0 ] && [ "$LINT_ERRORS" -eq 0 ]
