#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$ROOT/src/RTSP_Create.c"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

cc -std=c11 -Wall -Wextra -Werror -I"$ROOT/lnc" \
    "$ROOT/src/latency_profile.c" "$ROOT/tests/test_latency_profile.c" \
    -o "$TMPDIR/test_latency_profile"
"$TMPDIR/test_latency_profile"

grep -Eq 'src/latency_profile\.c' "$ROOT/Makefile"
grep -Eq 'test_latency_profile\.sh' "$ROOT/Makefile"
grep -Eq '#include "latency_profile\.h"' "$SOURCE"
grep -Eq 'latency_profile_observe' "$SOURCE"
grep -Eq '\[latency:profile\]' "$SOURCE"

echo 'latency profile source rules passed'
