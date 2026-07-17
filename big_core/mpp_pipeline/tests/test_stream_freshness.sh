#!/bin/sh
set -eu

root=${1:-.}
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

gcc -Wall -Wextra \
    -I"$root/big_core/mpp_pipeline" \
    -I/home/ubuntu/k230_sdk/src/big/mpp/include \
    "$root/big_core/mpp_pipeline/stream_freshness.c" \
    "$root/big_core/mpp_pipeline/tests/test_stream_freshness.c" \
    -o "$tmpdir/test_stream_freshness"

"$tmpdir/test_stream_freshness"
