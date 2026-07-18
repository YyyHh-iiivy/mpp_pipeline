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

preprocess_ai_off()
{
    sed '/^[[:space:]]*#include[[:space:]]/d' "$1" |
        ${CPP:-cpp} -P -undef -DAI_BRANCH_ENABLE=0
}

reject_active_pattern()
{
    source_file=$1
    pattern=$2
    message=$3

    if preprocess_ai_off "$source_file" | grep -Eq "$pattern"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_BRANCH_ENABLE[[:space:]]+0([[:space:]]|$)' \
    'the diagnostic build must default AI_BRANCH_ENABLE to 0'
require_pattern "$pipeline_file" \
    '\[diag\].*ai_branch=%u' \
    'the startup fingerprint must report ai_branch'
require_pattern "$vb_file" \
    'config\.max_pool_cnt[[:space:]]*=[[:space:]]*2;' \
    'the AI-off VB path must configure exactly two common pools'

reject_active_pattern "$vb_file" \
    'comm_pool\[2\]|AI_BUF_CNT|AI_CHN_BUF_SIZE' \
    'the AI-off VB path must not configure the AI pool'
reject_active_pattern "$vicap_file" \
    'AI_VICAP_CHN|AI_WIDTH|AI_HEIGHT|AI_BUF_CNT|AI_CHN_BUF_SIZE|AI_PIXEL_FORMAT' \
    'the AI-off VICAP path must not configure channel 2'
reject_active_pattern "$pipeline_file" \
    'ai_motion_thread_start[[:space:]]*\(' \
    'the AI-off main path must not initialize or start the motion thread'
reject_active_pattern "$cleanup_file" \
    'ai_motion_thread_stop[[:space:]]*\(' \
    'the AI-off cleanup path must not enter the motion-thread lifecycle'

echo 'AI branch disabled rules passed'
