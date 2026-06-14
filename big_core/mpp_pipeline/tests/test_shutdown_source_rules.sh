#!/bin/sh
set -eu

root=${1:-.}

handler=$(sed -n '/static void sig_handler/,/^}/p' "$root/big_core/mpp_pipeline/mpp_pipeline.c")
if printf '%s\n' "$handler" | grep -Eq 'LOG\(|printf\('; then
    echo "sig_handler must not call LOG/printf"
    exit 1
fi

if grep -Eq 'RT_WAITING_FOREVER' \
    "$root/big_core/mpp_pipeline/mpp_pipeline.c" \
    "$root/big_core/mpp_pipeline/motion_thread.c"; then
    echo "shutdown waits must be bounded, not RT_WAITING_FOREVER"
    exit 1
fi

echo "shutdown source rules passed"
