#!/bin/sh

set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
pipeline_file="$root/big_core/mpp_pipeline/mpp_pipeline.c"
osd_file="$root/big_core/mpp_pipeline/media_osd.c"
motion_file="$root/big_core/mpp_pipeline/motion_thread.c"

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

reject_text()
{
    text_to_check=$1
    pattern=$2
    message=$3

    if printf '%s\n' "$text_to_check" | grep -Eq "$pattern"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$pipeline_file" \
    '\[diag\] experiment=noosd_ab venc_osd=0 runtime_buffer_writes=0' \
    'the no-OSD build fingerprint is missing'
require_pattern "$motion_file" \
    '\[event:motion\].*osd_enabled=%u.*snapshot_ret=' \
    'motion events must report osd_enabled while preserving snapshot results'

disabled_osd=$(awk '
    /^#else \/\* VENC_OSD_ENABLE \*\// { disabled = 1; next }
    disabled && /^#endif \/\* VENC_OSD_ENABLE \*\// { exit }
    disabled { print }
' "$osd_file")

if [ -z "$disabled_osd" ]; then
    echo 'media_osd.c must provide an explicit VENC_OSD_ENABLE=0 stub branch' >&2
    exit 1
fi

for function_name in osd_init osd_set_motion_visible osd_poll_auto_hide osd_deinit; do
    if ! printf '%s\n' "$disabled_osd" | grep -Eq "^[[:space:]]*(k_s32|void)[[:space:]]+$function_name\\(.*\\)"; then
        echo "disabled OSD branch is missing $function_name stub" >&2
        exit 1
    fi
done

reject_text "$disabled_osd" \
    'kd_mpi_venc_(attach_2d|detach_2d|set_2d)|kd_mpi_vb_|kd_mpi_sys_(mmap|munmap|mmz_flush_cache)' \
    'disabled OSD stubs must not call VENC 2D, VB, mmap, or cache APIs'
reject_text "$disabled_osd" \
    'memcpy|memset|g_motion_detected_osd_argb8888|g_osd_' \
    'disabled OSD stubs must not write or retain runtime OSD buffer state'

snapshot_block=$(sed -n '/if (event.request_snapshot)/,/snapshot_ret = stream_export_request_snapshot/p' "$motion_file")
if [ -z "$snapshot_block" ] || printf '%s\n' "$snapshot_block" | grep -q 'VENC_OSD_ENABLE'; then
    echo 'snapshot requests must remain independent of VENC_OSD_ENABLE' >&2
    exit 1
fi

echo 'no-OSD A/B experiment rules passed'
