#!/bin/sh
set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
vb_file="$root/big_core/mpp_pipeline/media_vb.c"
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

preprocess_vb() {
    ai=$1
    osd=$2
    osd_index=$((2 + ai))
    sed '/^[[:space:]]*#include[[:space:]]/d' "$vb_file" |
        ${CPP:-cpp} -P -undef \
            -DAI_BRANCH_ENABLE="$ai" \
            -DVENC_OSD_ENABLE="$osd" \
            -DINPUT_BUF_CNT=3 -DOUTPUT_BUF_CNT=3 -DAI_BUF_CNT=6 \
            -DFRAME_BUF_SIZE=1 -DSTREAM_BUF_SIZE=2 -DAI_CHN_BUF_SIZE=3 \
            -DOSD_BUF_CNT=1 -DOSD_BUF_SIZE=4 \
            -DOSD_POOL_INDEX="$osd_index" \
            -DVB_REMAP_MODE_NOCACHE=0
}

assert_combo() {
    ai=$1
    osd=$2
    expected_pools=$3
    expected_osd_index=$4
    output=$(preprocess_vb "$ai" "$osd")

    printf '%s\n' "$output" | grep -Eq \
        "config\.max_pool_cnt[[:space:]]*=[[:space:]]*$expected_pools;" || {
        echo "AI=$ai OSD=$osd must configure $expected_pools pools" >&2
        exit 1
    }

    if [ "$osd" -eq 1 ]; then
        printf '%s\n' "$output" | grep -Eq \
            "config\.comm_pool\[$expected_osd_index\]\.blk_cnt[[:space:]]*=[[:space:]]*1;" || {
            echo "AI=$ai OSD=$osd must reserve OSD pool index $expected_osd_index" >&2
            exit 1
        }
    elif printf '%s\n' "$output" | grep -Eq 'OSD_BUF_(CNT|SIZE)'; then
        echo "AI=$ai OSD=$osd must not configure an OSD pool" >&2
        exit 1
    fi
}

require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+OSD_BUF_CNT[[:space:]]+1([[:space:]]|$)' \
    'OSD must reserve exactly one persistent VB block'
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+OSD_BUF_SIZE' \
    'OSD buffer size must be shared by VB and OSD modules'
require_pattern "$osd_file" \
    'kd_mpi_vb_handle_to_pool_id' \
    'OSD init must report the pool selected for its block'

assert_combo 0 0 2 0
assert_combo 1 0 3 0
assert_combo 0 1 3 2
assert_combo 1 1 4 3

echo 'OSD VB pool rules passed'
