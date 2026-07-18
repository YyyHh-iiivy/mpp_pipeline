#!/bin/sh
set -eu

root=${1:-.}
sconscript="$root/big_core/SConscript"

require_pattern() {
    file=$1
    pattern=$2
    message=$3
    if ! grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$sconscript" \
    "'AI_BRANCH_ENABLE'" \
    "module SConscript must accept an AI_BRANCH_ENABLE variant override"
require_pattern "$sconscript" \
    "'AI_MOTION_THREAD_ENABLE'" \
    "module SConscript must accept an AI_MOTION_THREAD_ENABLE variant override"
require_pattern "$sconscript" \
    "'VENC_OSD_ENABLE'" \
    "module SConscript must accept a VENC_OSD_ENABLE variant override"
require_pattern "$sconscript" \
    "ARGUMENTS\.get\\(macro_name\\)" \
    "module SConscript must read variant arguments"
require_pattern "$sconscript" \
    "CPPDEFINES" \
    "module SConscript must pass variant overrides as preprocessor definitions"

echo 'variant build rules passed'
