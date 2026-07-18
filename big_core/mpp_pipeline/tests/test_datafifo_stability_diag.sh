#!/bin/sh

set -eu

ROOT=./big_core/mpp_pipeline
IPC_FILE="$ROOT/stream_nalu_ipc.c"
EXPORT_FILE="$ROOT/stream_export.c"
VENC_FILE="$ROOT/media_venc.c"

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

require_pattern "$IPC_FILE" '\[ipc:submit-fail\].*stage=%s' \
    'DATAFIFO submit failures must identify their exact stage'
require_pattern "$IPC_FILE" 'last_read_done_seq=.*read_done_age_ms=' \
    'DATAFIFO submit failure diagnostics must include release progress'
require_pattern "$IPC_FILE" '\[ipc:drain\].*pending_before=.*pending_after=' \
    'DATAFIFO drain diagnostics must report pending progress'
require_pattern "$EXPORT_FILE" '\[export:fail\].*stage=' \
    'stream export early returns must identify their failure stage'
require_pattern "$VENC_FILE" 'status_cur_packs=.*output_pack_cnt=.*release_by_caller=' \
    'VENC submit failure diagnostics must include pack and ownership state'

submit_failure_block=$(awk '
    /ret = stream_export_submit_venc_stream/ { capture = 1 }
    capture { print }
    capture && /if \(release_by_caller\)/ { exit }
' "$VENC_FILE")
if printf '%s\n' "$submit_failure_block" | grep -q 'stream_export_flush'; then
    echo 'The diagnostic build must not add recovery flush after submit failure' >&2
    exit 1
fi

echo 'DATAFIFO stability diagnostic rules passed'
