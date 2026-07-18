#!/bin/sh
set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
vb_file="$root/big_core/mpp_pipeline/media_vb.c"
vicap_file="$root/big_core/mpp_pipeline/media_vicap.c"
pipeline_file="$root/big_core/mpp_pipeline/mpp_pipeline.c"
cleanup_file="$root/big_core/mpp_pipeline/media_cleanup.c"
osd_file="$root/big_core/mpp_pipeline/media_osd.c"

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
    '^[[:space:]]*#define[[:space:]]+AI_BRANCH_ENABLE[[:space:]]+1([[:space:]]|$)' \
    'the default build must enable the AI branch'
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+VENC_OSD_ENABLE[[:space:]]+1([[:space:]]|$)' \
    'the default build must enable hardware OSD'
require_pattern "$vb_file" \
    'config\.max_pool_cnt[[:space:]]*=[[:space:]]*3;' \
    'the AI-enabled VB path must configure the AI pool'
require_pattern "$vicap_file" \
    'vicap_set_channel_attr\(AI_VICAP_CHN,[[:space:]]*AI_WIDTH,[[:space:]]*AI_HEIGHT' \
    'the AI-enabled VICAP path must configure channel 2'
require_pattern "$pipeline_file" \
    'ai_motion_thread_start\(\)' \
    'the default pipeline must start the AI motion thread'
require_pattern "$cleanup_file" \
    'ai_motion_thread_stop\(\)' \
    'the default cleanup path must stop the AI motion thread'
require_pattern "$osd_file" \
    'VENC 2D OSD init OK' \
    'the hardware OSD implementation must remain compiled in'

echo 'AI and hardware OSD enabled rules passed'
