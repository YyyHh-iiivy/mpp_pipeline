#!/bin/sh
set -eu

root=${1:-.}
top_sconscript="$root/big_core/SConscript"
module_sconscript="$root/big_core/mpp_pipeline/SConscript"
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
ipc_header="$root/big_core/mpp_pipeline/mpp_nalu_ipc.h"
ipc_file="$root/big_core/mpp_pipeline/stream_nalu_ipc.c"
export_file="$root/big_core/mpp_pipeline/stream_export.c"
motion_thread_file="$root/big_core/mpp_pipeline/motion_thread.c"
pipeline_file="$root/big_core/mpp_pipeline/mpp_pipeline.c"
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

reject_pattern() {
    file=$1
    pattern=$2
    message=$3

    if grep -Eq "$pattern" "$file"; then
        echo "$message"
        exit 1
    fi
}

require_pattern "$top_sconscript" "'mpp_pipeline/stream_freshness\.c'" \
    "top-level SConscript must compile stream_freshness.c"
require_pattern "$module_sconscript" "'stream_freshness\.c'" \
    "module SConscript must list stream_freshness.c"

require_pattern "$header" '^[[:space:]]*#define[[:space:]]+OUTPUT_BUF_CNT[[:space:]]+3([[:space:]]|$)' \
    "OUTPUT_BUF_CNT must be 3 for the low-latency RTSP profile"
require_pattern "$ipc_file" '^[[:space:]]*#define[[:space:]]+NALU_IPC_PENDING_MAX[[:space:]]+2U?([[:space:]]|$)' \
    "NALU_IPC_PENDING_MAX must cap outstanding DATAFIFO streams at 2"
require_pattern "$ipc_file" '^[[:space:]]*#define[[:space:]]+NALU_IPC_FIFO_ENTRIES[[:space:]]+3U?([[:space:]]|$)' \
    "DATAFIFO entries must remain 3 for small-core ring compatibility"
require_pattern "$ipc_header" 'submit_time_ms' \
    "mpp_nalu_ipc_msg must carry big-core submit_time_ms for latency tracing"
require_pattern "$ipc_header" 'MPP_NALU_IPC_FLAG_SNAPSHOT[[:space:]]+\(1U[[:space:]]*<<[[:space:]]*0\)' \
    "mpp_nalu_ipc.h must define the shared snapshot flag bit"
require_pattern "$ipc_file" 'nalu_ipc_drop_current_stream' \
    "DATAFIFO backend must explicitly drop the current stream when congested"
require_pattern "$ipc_file" 'nalu_ipc_log_stats' \
    "DATAFIFO backend must expose low-frequency pending/drop statistics"
require_pattern "$ipc_file" 'nalu_ipc_drain_releases' \
    "DATAFIFO backend must actively drain reader READ_DONE notifications"
require_pattern "$ipc_file" 'NALU_IPC_DRAIN_MAX_PASSES' \
    "DATAFIFO drain must have a bounded retry cap"
require_pattern "$ipc_file" 'nalu_ipc_log_pending_slots' \
    "DATAFIFO backend must log pending slot ages for stuck pending diagnosis"
require_pattern "$ipc_file" 'pending_full_after_drain' \
    "DATAFIFO pending_full path must retry release drain before dropping"
require_pattern "$ipc_file" 'return[[:space:]]+nalu_ipc_drain_releases\(\)' \
    "nalu_ipc_flush must reuse the bounded release drain helper"
require_pattern "$ipc_file" 'seq_gap=' \
    "DATAFIFO stats must expose seq_gap for big/small-core seq alignment"
require_pattern "$ipc_file" 'read_done_age_ms=' \
    "DATAFIFO stats must expose READ_DONE latency from submit_time_ms"
require_pattern "$ipc_file" 'pending_high_water=' \
    "DATAFIFO stats must expose pending high-water depth"
require_pattern "$ipc_file" 'Low-latency IPC config:' \
    "DATAFIFO init must log the pending and FIFO configuration"
require_pattern "$ipc_file" 'NALU IPC READ_DONE' \
    "DATAFIFO release callback must log READ_DONE latency by seq"
require_pattern "$ipc_file" 'k_s32[[:space:]]+nalu_ipc_submit_stream\(k_u32 chn,' \
    "DATAFIFO submit API must keep the expected function signature"
require_pattern "$ipc_file" 'k_u32 flags\)' \
    "DATAFIFO submit API must accept reserved flags from the export layer"
require_pattern "$ipc_file" 'msg->reserved[[:space:]]*=[[:space:]]*flags' \
    "DATAFIFO IPC message must carry snapshot flags in reserved"
require_pattern "$ipc_file" '\[big\] snapshot flag set seq=' \
    "DATAFIFO backend must log when it submits a snapshot-flagged stream"
require_pattern "$export_file" 'stream_export_request_snapshot' \
    "stream export layer must expose snapshot request queueing"
require_pattern "$export_file" 'SNAPSHOT_PENDING_MAX[[:space:]]+4U' \
    "snapshot request queue must hold four pending requests"
require_pattern "$export_file" 'SNAPSHOT_WAIT_IDR_FRAMES[[:space:]]+\(VENC_GOP[[:space:]]*\+[[:space:]]*2U\)' \
    "snapshot selection must wait for an IDR/header frame before falling back"
reject_pattern "$export_file" 'kd_mpi_venc_request_idr' \
    "snapshot queueing must not request an immediate IDR"
require_pattern "$export_file" 'Snapshot mock IPC event' \
    "local-log export mode must emit a mock snapshot IPC event"
require_pattern "$motion_thread_file" 'stream_export_request_snapshot' \
    "AI motion thread must request a snapshot after a motion event"
reject_pattern "$venc_file" 'kd_mpi_venc_enable_idr' \
    "VENC channel creation must not enable runtime IDR requests"
require_pattern "$venc_file" 'stream_export_get_pending_count' \
    "stream_thread stats must include DATAFIFO pending depth"
require_pattern "$venc_file" 'stream_freshness_observe' \
    "stream thread must evaluate source-frame freshness"
require_pattern "$venc_file" 'stale_drop' \
    "stream thread must report stale-frame drops"
require_pattern "$venc_file" 'VENC empty stream:' \
    "stream thread must diagnose successful get_stream calls that return zero packs"
require_pattern "$vb_file" '3\*4MB \+ 3\*1MB' \
    "VB memory comment must document the reduced VENC stream pool size"

release_progress_order=$(awk '
    /while \(g_stream_running\)/ { in_loop = 1 }
    in_loop && /stream_export_flush\(\)/ && flush_line == 0 { flush_line = NR }
    in_loop && /kd_mpi_venc_query_status/ && query_line == 0 { query_line = NR }
    END {
        if (flush_line > 0 && query_line > 0 && flush_line < query_line) {
            print "ok";
        } else {
            print "bad";
        }
    }
' "$venc_file")
if [ "$release_progress_order" != "ok" ]; then
    echo "stream thread must drain DATAFIFO releases before querying VENC"
    exit 1
fi

empty_stream_order=$(awk '
    /kd_mpi_venc_get_stream/ { get_line = NR }
    get_line > 0 && NR > get_line && /if \(output\.pack_cnt == 0\)/ && empty_line == 0 {
        empty_line = NR
    }
    /stream_export_submit_venc_stream/ && submit_line == 0 { submit_line = NR }
    END {
        if (get_line > 0 && empty_line > get_line &&
            submit_line > empty_line) {
            print "ok";
        } else {
            print "bad";
        }
    }
' "$venc_file")
if [ "$empty_stream_order" != "ok" ]; then
    echo "stream thread must reject zero-pack streams after get_stream and before export"
    exit 1
fi

pipeline_order=$(awk '
    /stream_export_init\(STREAM_EXPORT_DATAFIFO\)/ { export_line = NR }
    /ai_motion_thread_start\(\)/ { ai_line = NR }
    END {
        if (export_line > 0 && ai_line > 0 && export_line < ai_line) {
            print "ok";
        } else {
            print "bad";
        }
    }
' "$pipeline_file")
if [ "$pipeline_order" != "ok" ]; then
    echo "stream_export_init must run before ai_motion_thread_start"
    exit 1
fi

motion_event_order=$(awk '
    /static void ai_motion_thread\(/ { in_function = 1 }
    in_function && /osd_set_motion_visible\(1,/ { osd_line = NR }
    in_function && /stream_export_request_snapshot\(&snapshot_req\)/ { snapshot_line = NR }
    in_function && /^}/ {
        if (osd_line > 0 && snapshot_line > osd_line) {
            print "ok";
        } else {
            print "bad";
        }
        exit;
    }
' "$motion_thread_file")
if [ "$motion_event_order" != "ok" ]; then
    echo "AI motion event must show OSD before queueing the snapshot request"
    exit 1
fi

echo "low latency DATAFIFO rules passed"
