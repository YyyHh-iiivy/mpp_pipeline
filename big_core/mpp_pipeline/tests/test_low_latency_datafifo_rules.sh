#!/bin/sh
set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
ipc_header="$root/big_core/mpp_pipeline/mpp_nalu_ipc.h"
ipc_file="$root/big_core/mpp_pipeline/stream_nalu_ipc.c"
vb_file="$root/big_core/mpp_pipeline/media_vb.c"
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

require_pattern "$header" '^[[:space:]]*#define[[:space:]]+OUTPUT_BUF_CNT[[:space:]]+4([[:space:]]|$)' \
    "OUTPUT_BUF_CNT must be 4 for the low-latency RTSP profile"
require_pattern "$ipc_file" '^[[:space:]]*#define[[:space:]]+NALU_IPC_PENDING_MAX[[:space:]]+3U?([[:space:]]|$)' \
    "NALU_IPC_PENDING_MAX must cap outstanding DATAFIFO streams at 3"
require_pattern "$ipc_file" '^[[:space:]]*#define[[:space:]]+NALU_IPC_FIFO_ENTRIES[[:space:]]+NALU_IPC_PENDING_MAX([[:space:]]|$)' \
    "DATAFIFO entries must track the low-latency pending cap"
require_pattern "$ipc_header" 'submit_time_ms' \
    "mpp_nalu_ipc_msg must carry big-core submit_time_ms for latency tracing"
require_pattern "$ipc_file" 'nalu_ipc_drop_current_stream' \
    "DATAFIFO backend must explicitly drop the current stream when congested"
require_pattern "$ipc_file" 'nalu_ipc_log_stats' \
    "DATAFIFO backend must expose low-frequency pending/drop statistics"
require_pattern "$venc_file" 'stream_export_get_pending_count' \
    "stream_thread stats must include DATAFIFO pending depth"
require_pattern "$vb_file" '6\*4MB \+ 4\*1MB' \
    "VB memory comment must document the reduced VENC stream pool size"

echo "low latency DATAFIFO rules passed"
