#!/bin/sh

set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
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

require_pattern "$header" \
    '^[[:space:]]*#ifndef[[:space:]]+AI_MOTION_ACQUIRE_FPS' \
    "AI motion FPS must be overridable"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_ACQUIRE_FPS[[:space:]]+5([[:space:]]|$)' \
    "AI motion FPS must default to 5"
require_pattern "$header" \
    'AI_MOTION_ACQUIRE_INTERVAL_MS[[:space:]]+\(1000U[[:space:]]*/[[:space:]]*AI_MOTION_ACQUIRE_FPS\)' \
    "AI motion interval must be derived from the selected FPS"
require_pattern "$header" \
    '^[[:space:]]*#ifndef[[:space:]]+MOTION_STATE_DIAG_ENABLE' \
    "motion state diagnostics must be overridable"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+MOTION_STATE_DIAG_ENABLE[[:space:]]+0([[:space:]]|$)' \
    "motion state diagnostics must default to disabled"

require_pattern "$sconscript" \
    "ARGUMENTS\.get\('AI_MOTION_ACQUIRE_FPS'\)" \
    "SConscript must read AI_MOTION_ACQUIRE_FPS"
require_pattern "$sconscript" \
    "AI_MOTION_ACQUIRE_FPS must be 5 or 10" \
    "SConscript must reject unsupported AI motion FPS values"
require_pattern "$sconscript" \
    "ARGUMENTS\.get\('MOTION_STATE_DIAG_ENABLE'\)" \
    "SConscript must read MOTION_STATE_DIAG_ENABLE"
require_pattern "$sconscript" \
    "MOTION_STATE_DIAG_ENABLE must be 0 or 1" \
    "SConscript must reject unsupported diagnostic values"
require_pattern "$sconscript" \
    'CPPDEFINES[[:space:]]*=[[:space:]]*variant_defines' \
    "SConscript must pass variant definitions to the compiler"

echo "motion diagnostic build rules passed"
