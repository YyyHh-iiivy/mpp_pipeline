#!/bin/sh

set -eu

root=${1:-.}
module_header="$root/big_core/mpp_pipeline/compact_diag.h"
module_source="$root/big_core/mpp_pipeline/compact_diag.c"
venc_file="$root/big_core/mpp_pipeline/media_venc.c"
ipc_file="$root/big_core/mpp_pipeline/stream_nalu_ipc.c"
osd_file="$root/big_core/mpp_pipeline/media_osd.c"
motion_file="$root/big_core/mpp_pipeline/motion_thread.c"
export_file="$root/big_core/mpp_pipeline/stream_export.c"
top_sconscript="$root/big_core/SConscript"
module_sconscript="$root/big_core/mpp_pipeline/SConscript"
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

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

if [ ! -f "$module_header" ] || [ ! -f "$module_source" ]; then
    echo 'compact_diag production module is missing' >&2
    exit 1
fi

gcc -std=c11 -Wall -Wextra -Werror \
    -I"$root/big_core/mpp_pipeline" \
    "$module_source" \
    "$root/big_core/mpp_pipeline/tests/test_compact_diag.c" \
    -o "$tmpdir/test_compact_diag"
"$tmpdir/test_compact_diag"

require_pattern "$module_header" 'COMPACT_DIAG_NORMAL_INTERVAL_MS[[:space:]]+60000ULL' \
    'normal compact diagnostics must use a 60-second interval'
require_pattern "$module_header" 'COMPACT_DIAG_STALL_THRESHOLD_MS[[:space:]]+500ULL' \
    'VENC stall diagnostics must start after 500ms without a valid stream'
require_pattern "$module_header" 'COMPACT_DIAG_ANOMALY_INTERVAL_MS[[:space:]]+1000ULL' \
    'ongoing stall diagnostics must use a one-second interval'
require_pattern "$top_sconscript" "'mpp_pipeline/compact_diag\.c'" \
    'top-level SConscript must compile compact_diag.c'
require_pattern "$module_sconscript" "'compact_diag\.c'" \
    'module SConscript must compile compact_diag.c'

require_pattern "$venc_file" '\[diag\] compact_diag normal=60s stall=500ms anomaly=1s' \
    'big-core compact diagnostic version fingerprint is missing'
require_pattern "$venc_file" '\[health:venc\].*uptime_s=.*fps=.*interval_bytes=.*total_frames=.*last_pts_age_ms=.*pending=.*stale_drop=' \
    'VENC 60-second health summary is missing required fields'
require_pattern "$venc_file" '\[stall:venc\].*state=%s.*cause=%s.*ret=0x%x.*elapsed_ms=.*last_seq=.*last_pts_age_ms=.*cur_packs=.*pic_cnt=.*pic_bytes=.*mean_qp=.*pending=' \
    'VENC adaptive stall diagnostic is missing required fields'
require_pattern "$venc_file" 'compact_diag_observe' \
    'stream thread must drive the compact stall state machine'
reject_pattern "$venc_file" 'kd_mpi_venc_query_status failed' \
    'query failures must be emitted only through the one-second adaptive stall log'
reject_pattern "$venc_file" 'kd_mpi_venc_get_stream timeout/no stream' \
    'get_stream failures must be emitted only through the one-second adaptive stall log'

require_pattern "$ipc_file" '\[health:ipc\].*seq=.*pending=.*pending_high_water=.*read_done_seq=.*read_done_age_ms=.*drop_current=.*datafifo_full=.*pending_full=.*write_fail=' \
    'IPC 60-second health summary is missing required fields'
require_pattern "$ipc_file" 'NALU_IPC_READ_DONE_SLOW_MS[[:space:]]+500ULL' \
    'normal READ_DONE callbacks must stay quiet below 500ms'
require_pattern "$ipc_file" 'NALU_IPC_TIMING_ABNORMAL_US[[:space:]]+250000ULL' \
    'VENC timing diagnostics must ignore normal 66/133ms cadence'
require_pattern "$ipc_file" 'NALU_IPC_DRAIN_STALL_MS[[:space:]]+500ULL' \
    'IPC drain diagnostics must ignore healthy short pending lifetimes'
reject_pattern "$ipc_file" 'NALU_IPC_STATS_INTERVAL[[:space:]]+150' \
    'IPC health must be time-based, not emitted every 150 frames'

require_pattern "$motion_file" '\[event:motion\].*id=.*score=.*osd_ret=.*snapshot_ret=' \
    'motion event logging must be coalesced into one result line'
reject_pattern "$export_file" 'Snapshot request queued:' \
    'successful snapshot queueing must not emit a separate line'
reject_pattern "$export_file" 'Snapshot request delivered to DATAFIFO:' \
    'successful snapshot delivery must not emit a separate line'
reject_pattern "$ipc_file" '\[big\] snapshot flag set' \
    'successful snapshot flag propagation must stay quiet'
reject_pattern "$osd_file" 'OSD auto hide visible=0' \
    'successful OSD hide must not emit a redundant line'

echo 'compact diagnostic source rules passed'
