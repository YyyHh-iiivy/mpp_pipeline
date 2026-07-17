#!/bin/sh
set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
vicap_file="$root/big_core/mpp_pipeline/media_vicap.c"
venc_file="$root/big_core/mpp_pipeline/media_venc.c"

require_pattern() {
    file=$1
    pattern=$2
    message=$3

    if ! grep -Eq "$pattern" "$file"; then
        echo "$message"
        exit 1
    fi
}

require_pattern "$header" '^[[:space:]]*#define[[:space:]]+SRC_FPS[[:space:]]+30([[:space:]]|$)' \
    "SRC_FPS must document the 30fps sensor/input baseline"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+DST_FPS[[:space:]]+15([[:space:]]|$)' \
    "DST_FPS must be 15fps for the low-latency RTSP profile"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VICAP_OUTPUT_FPS[[:space:]]+15([[:space:]]|$)' \
    "VICAP_OUTPUT_FPS must request 15fps VICAP channel output"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VENC_FORCE_IDR_ENABLE[[:space:]]+0([[:space:]]|$)' \
    "runtime forced IDR must remain disabled by default"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VENC_GOP[[:space:]]+8([[:space:]]|$)' \
    "natural VENC_GOP fallback must remain 8 for the measured low-latency profile"

require_pattern "$vicap_file" 'chn_attr\.fps[[:space:]]*=[[:space:]]*VICAP_OUTPUT_FPS;' \
    "VICAP channel attr must apply VICAP_OUTPUT_FPS"
require_pattern "$venc_file" 'attr\.rc_attr\.cbr\.gop[[:space:]]*=[[:space:]]*VENC_GOP;' \
    "VENC GOP must use VENC_GOP instead of a hard-coded frame count"

echo "frame rate config rules passed"
