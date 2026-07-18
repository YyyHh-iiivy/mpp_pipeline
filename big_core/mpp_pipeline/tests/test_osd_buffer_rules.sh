#!/bin/sh

set -eu

root=${1:-.}
osd_file="$root/big_core/mpp_pipeline/media_osd.c"
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

set_param_count=$(grep -Ec '^[[:space:]]*ret[[:space:]]*=[[:space:]]*kd_mpi_venc_set_2d_osd_param' "$osd_file")
if [ "$set_param_count" -ne 1 ]; then
    echo 'kd_mpi_venc_set_2d_osd_param must be called exactly once' >&2
    exit 1
fi

osd_init_body=$(sed -n '/^k_s32 osd_init(void)/,/^}/p' "$osd_file")
if ! printf '%s\n' "$osd_init_body" | grep -q 'kd_mpi_venc_set_2d_osd_param'; then
    echo 'the sole VENC OSD region configuration must remain in osd_init' >&2
    exit 1
fi
if ! printf '%s\n' "$osd_init_body" | grep -Eq 'g_osd_attr\.osd_alpha[[:space:]]*=[[:space:]]*255'; then
    echo 'osd_init must configure the fixed region alpha to 255' >&2
    exit 1
fi

update_body=$(sed -n '/^static k_s32 osd_update_buffer(k_bool visible)/,/^}/p' "$osd_file")
if [ -z "$update_body" ]; then
    echo 'runtime OSD updates must use osd_update_buffer' >&2
    exit 1
fi
if ! printf '%s\n' "$update_body" | grep -q 'memcpy(g_osd_virt_addr'; then
    echo 'visible OSD updates must copy the ARGB asset into the MMZ buffer' >&2
    exit 1
fi
if ! printf '%s\n' "$update_body" | grep -q 'g_motion_detected_osd_argb8888'; then
    echo 'visible OSD updates must use the generated motion asset' >&2
    exit 1
fi
if ! printf '%s\n' "$update_body" | grep -q 'memset(g_osd_virt_addr, 0, OSD_BUF_SIZE)'; then
    echo 'hidden OSD updates must clear the whole MMZ buffer' >&2
    exit 1
fi
if ! printf '%s\n' "$update_body" | grep -q 'kd_mpi_sys_mmz_flush_cache'; then
    echo 'every runtime OSD buffer update must flush the MMZ cache' >&2
    exit 1
fi
if printf '%s\n' "$update_body" | grep -q 'kd_mpi_venc_set_2d_osd_param'; then
    echo 'runtime OSD buffer updates must not reconfigure the VENC region' >&2
    exit 1
fi

poll_body=$(sed -n '/^k_s32 osd_poll_auto_hide(void)/,/^}/p' "$osd_file")
if ! printf '%s\n' "$poll_body" | grep -q 'osd_update_buffer(target_visible)'; then
    echo 'osd_poll_auto_hide must apply pending visibility through the MMZ buffer' >&2
    exit 1
fi
if printf '%s\n' "$poll_body" | grep -q 'kd_mpi_venc_set_2d_osd_param'; then
    echo 'osd_poll_auto_hide must not call the runtime VENC 2D parameter API' >&2
    exit 1
fi

require_pattern "$osd_file" '\[osd:buffer\].*generation=.*visible=.*cost_ms=.*ret=.*pending_after=' \
    'OSD buffer diagnostics must include generation, visibility, cost, result, and pending state'
reject_pattern "$osd_file" '\[osd:apply\]' \
    'the obsolete runtime VENC parameter log must be removed'

stream_body=$(sed -n '/^void stream_thread(void \*arg)/,/^}/p' "$venc_file")
poll_line=$(printf '%s\n' "$stream_body" | grep -n 'osd_poll_auto_hide();' | head -n1 | cut -d: -f1)
query_line=$(printf '%s\n' "$stream_body" | grep -n 'kd_mpi_venc_query_status' | head -n1 | cut -d: -f1)
get_line=$(printf '%s\n' "$stream_body" | grep -n 'kd_mpi_venc_get_stream' | head -n1 | cut -d: -f1)
if [ -z "$poll_line" ] || [ -z "$query_line" ] || [ -z "$get_line" ] || \
   [ "$poll_line" -ge "$query_line" ] || [ "$poll_line" -ge "$get_line" ]; then
    echo 'stream thread must apply pending OSD buffers before VENC query/get' >&2
    exit 1
fi

echo 'OSD buffer rules passed'
