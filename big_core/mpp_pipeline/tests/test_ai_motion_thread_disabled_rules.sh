#!/bin/sh

set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
vb_file="$root/big_core/mpp_pipeline/media_vb.c"
vicap_file="$root/big_core/mpp_pipeline/media_vicap.c"
pipeline_file="$root/big_core/mpp_pipeline/mpp_pipeline.c"
cleanup_file="$root/big_core/mpp_pipeline/media_cleanup.c"

require_pattern()
{
    file=$1
    pattern=$2
    message=$3

    if ! grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

preprocess_channel_only()
{
    sed '/^[[:space:]]*#include[[:space:]]/d' "$1" |
        ${CPP:-cpp} -P -undef \
            -DAI_BRANCH_ENABLE=1 \
            -DAI_MOTION_THREAD_ENABLE=0 \
            -DVENC_OSD_ENABLE=0
}

require_active_pattern()
{
    source_file=$1
    pattern=$2
    message=$3

    if ! preprocess_channel_only "$source_file" | grep -Eq "$pattern"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_active_pattern()
{
    source_file=$1
    pattern=$2
    message=$3

    if preprocess_channel_only "$source_file" | grep -Eq "$pattern"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$header" \
    '^[[:space:]]*#ifndef[[:space:]]+AI_MOTION_THREAD_ENABLE' \
    'AI_MOTION_THREAD_ENABLE must be independently overridable'
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_THREAD_ENABLE[[:space:]]+1([[:space:]]|$)' \
    'the default build must enable the AI motion thread'
require_pattern "$pipeline_file" \
    '\[diag\].*ai_branch=%u.*ai_motion_thread=%u' \
    'the startup fingerprint must report both independent AI switches'

require_active_pattern "$vb_file" \
    'comm_pool\[2\].*AI_BUF_CNT|comm_pool\[2\].*AI_CHN_BUF_SIZE' \
    'the channel-only build must retain the AI VB pool'
require_active_pattern "$vicap_file" \
    'vicap_set_channel_attr\(AI_VICAP_CHN,[[:space:]]*AI_WIDTH,[[:space:]]*AI_HEIGHT' \
    'the channel-only build must retain VICAP channel 2'
reject_active_pattern "$pipeline_file" \
    'ai_motion_thread_start[[:space:]]*\(' \
    'the channel-only main path must not start the AI motion thread'
reject_active_pattern "$cleanup_file" \
    'ai_motion_thread_stop[[:space:]]*\(' \
    'the channel-only cleanup path must not stop the AI motion thread'

echo 'AI motion thread disabled override rules passed'
