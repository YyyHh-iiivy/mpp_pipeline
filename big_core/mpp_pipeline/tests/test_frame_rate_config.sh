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

reject_pattern() {
    file=$1
    pattern=$2
    message=$3

    if grep -Eq "$pattern" "$file"; then
        echo "$message"
        exit 1
    fi
}

require_pattern "$header" '^[[:space:]]*#define[[:space:]]+SRC_FPS[[:space:]]+15([[:space:]]|$)' \
    "SRC_FPS must match the requested 15fps VICAP input cadence"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+DST_FPS[[:space:]]+15([[:space:]]|$)' \
    "DST_FPS must be 15fps for the low-latency RTSP profile"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VICAP_OUTPUT_FPS[[:space:]]+15([[:space:]]|$)' \
    "VICAP_OUTPUT_FPS must request 15fps VICAP channel output"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+ENC_BITRATE[[:space:]]+1500([[:space:]]|$)' \
    "ENC_BITRATE must use the 1500kbps low-latency, burst-limited profile"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VENC_HEALTH_INTERVAL_MS[[:space:]]+5000ULL([[:space:]]|$)' \
    "VENC health statistics must use a five-second interval"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VENC_FORCE_IDR_ENABLE[[:space:]]+0([[:space:]]|$)' \
    "runtime forced IDR must remain disabled by default"
require_pattern "$header" '^[[:space:]]*#define[[:space:]]+VENC_GOP[[:space:]]+8([[:space:]]|$)' \
    "natural VENC_GOP fallback must remain 8 for the measured low-latency profile"

require_pattern "$vicap_file" 'chn_attr\.fps[[:space:]]*=[[:space:]]*VICAP_OUTPUT_FPS;' \
    "VICAP channel attr must apply VICAP_OUTPUT_FPS"
require_pattern "$venc_file" 'attr\.rc_attr\.cbr\.gop[[:space:]]*=[[:space:]]*VENC_GOP;' \
    "VENC GOP must use VENC_GOP instead of a hard-coded frame count"
require_pattern "$venc_file" 'Low-latency VENC config: src_fps=%u dst_fps=%u vicap_fps=%u bitrate_kbps=%u gop=%u' \
    "VENC startup fingerprint must include the configured bitrate"
require_pattern "$venc_file" 'VENC_HEALTH_INTERVAL_MS' \
    "VENC stream health reporting must use its five-second interval"
require_pattern "$venc_file" '\[health:venc\].*fps=.*kbps=.*interval_bytes=.*avg_frame_bytes=.*max_frame_bytes=.*pts_delta_min_us=.*pts_delta_max_us=.*mean_qp=' \
    "VENC health summary must report bitrate, frame sizes, PTS cadence, and mean QP"
reject_pattern "$venc_file" 'VICAP.*30fps' \
    "VENC source-rate comments must not retain the obsolete 30fps VICAP assumption"

echo "frame rate config rules passed"
