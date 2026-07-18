#!/bin/sh

set -eu

root=${1:-.}
venc_file="$root/big_core/mpp_pipeline/media_venc.c"

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

reject_pattern()
{
    file=$1
    pattern=$2
    message=$3

    if grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern "$venc_file" \
    'output\.pack_cnt[[:space:]]*=[[:space:]]*VENC_MAX_PACKS' \
    'VENC get_stream capacity must never silently truncate status.cur_packs'
require_pattern "$venc_file" \
    'query_pack_cnt[[:space:]]*=[[:space:]]*status\.cur_packs[[:space:]]*\?[[:space:]]*status\.cur_packs[[:space:]]*:[[:space:]]*1U' \
    'VENC get_stream capacity must preserve the queried pack count'
require_pattern "$venc_file" \
    'calloc\(query_pack_cnt,[[:space:]]*sizeof\(\*dynamic_packs\)\)' \
    'VENC pack overflow path must allocate the exact queried descriptor count'
require_pattern "$venc_file" \
    '\[venc:pack-overflow\].*query_packs=.*returned_packs=.*total_bytes=.*pts=.*count=.*max_query_packs=' \
    'VENC pack overflow diagnostics must expose query, return, size, PTS, and counters'
require_pattern "$venc_file" \
    '\[health:venc\].*max_query_packs=.*pack_overflow_count=' \
    'VENC health summary must expose maximum queried packs and overflow count'

overflow_block=$(awk '
    /if \(query_pack_cnt > VENC_MAX_PACKS \|\|/ { capture = 1 }
    capture { print }
    capture && /continue;/ { exit }
' "$venc_file")

if [ -z "$overflow_block" ]; then
    echo 'VENC pack overflow handling block is missing' >&2
    exit 1
fi
if ! printf '%s\n' "$overflow_block" | grep -q 'kd_mpi_venc_release_stream'; then
    echo 'VENC pack overflow path must release the complete SDK stream' >&2
    exit 1
fi
if ! printf '%s\n' "$overflow_block" | grep -q 'free(dynamic_packs)'; then
    echo 'VENC pack overflow path must free temporary descriptors' >&2
    exit 1
fi
if printf '%s\n' "$overflow_block" | grep -q 'stream_export_submit_venc_stream'; then
    echo 'VENC pack overflow path must not enter the 8-pack DATAFIFO export protocol' >&2
    exit 1
fi

echo 'VENC pack capacity rules passed'
