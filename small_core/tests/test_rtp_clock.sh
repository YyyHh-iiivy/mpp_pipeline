#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_BIN=${TMPDIR:-/tmp}/k230_rtp_clock_test_$$

cleanup()
{
    rm -f "$TEST_BIN"
}
trap cleanup EXIT INT TERM

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/lnc" \
    "$ROOT/src/rtp_clock.c" \
    "$ROOT/tests/test_rtp_clock.c" \
    -o "$TEST_BIN"

"$TEST_BIN"
