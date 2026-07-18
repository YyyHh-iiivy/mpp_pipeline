#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$ROOT/src/RTSP_Create.c"
MAKEFILE="$ROOT/Makefile"

line_after()
{
    start=$1
    needle=$2
    awk -v start="$start" -v needle="$needle" \
        'NR > start && index($0, needle) != 0 { print NR; exit }' "$SOURCE"
}

require_order()
{
    first=$1
    second=$2
    message=$3

    if [ -z "$first" ] || [ -z "$second" ] || [ "$first" -ge "$second" ]; then
        echo "$message" >&2
        exit 1
    fi
}

rebase_if_line=$(grep -n 'if (rtp_clock.rebase_count != rebase_count_before)' "$SOURCE" | head -n1 | cut -d: -f1)
rebase_log_line=$(line_after "$rebase_if_line" '[rtp:rebase]')
wait_line=$(line_after "$rebase_if_line" 'wait_for_idr = 1;')
skip_reset_line=$(line_after "$rebase_if_line" 'g_wait_random_access_skip_count = 0;')
request_sets_line=$(line_after "$rebase_if_line" 'rtsp_request_parameter_sets();')
wait_log_line=$(line_after "$rebase_if_line" 'PTS rebase, waiting random access')
process_frame_line=$(line_after "$rebase_if_line" 'send_ret = datafifo_process_copied_frame_for_playback')

require_order "$rebase_if_line" "$rebase_log_line" \
    'PTS rebase must be logged when rebase_count increases'
require_order "$rebase_log_line" "$wait_line" \
    'PTS rebase must enter wait_for_idr before processing the current frame'
require_order "$wait_line" "$skip_reset_line" \
    'PTS rebase must reset the random-access skip counter'
require_order "$skip_reset_line" "$request_sets_line" \
    'PTS rebase must request cached parameter sets after resetting wait state'
require_order "$request_sets_line" "$wait_log_line" \
    'PTS rebase must log that it is waiting for natural random access'
require_order "$wait_log_line" "$process_frame_line" \
    'all PTS rebase recovery state must be set before the current H.265 frame is processed'

rebase_window=$(sed -n "${rebase_if_line},${process_frame_line}p" "$SOURCE")
if printf '%s\n' "$rebase_window" | grep -Eq 'ctrl_fifo_request_idr|rtp_clock_reset|seq[[:space:]]*=[[:space:]]*0'; then
    echo 'PTS rebase recovery must not request IDR or reset RTP identity/timestamp state' >&2
    exit 1
fi

wait_block_start=$(grep -n 'if (\*wait_for_idr)' "$SOURCE" | head -n1 | cut -d: -f1)
skip_vcl_line=$(line_after "$wait_block_start" 'h265_is_vcl(nal_type)')
random_access_line=$(line_after "$wait_block_start" 'h265_is_random_access(nal_type)')
send_sets_line=$(line_after "$wait_block_start" 'send_cached_parameter_sets(sock')
clear_wait_line=$(line_after "$wait_block_start" '*wait_for_idr = 0;')
random_start_line=$(line_after "$wait_block_start" '[rtp] random access start:')
send_nalu_line=$(line_after "$wait_block_start" 'send_ret = send_h265_nalu_rtp')

require_order "$random_access_line" "$skip_vcl_line" \
    'ordinary VCL must be skipped while waiting for natural random access'
require_order "$skip_vcl_line" "$send_sets_line" \
    'cached VPS/SPS/PPS must only be sent when random access arrives'
require_order "$send_sets_line" "$clear_wait_line" \
    'wait_for_idr must remain set until cached VPS/SPS/PPS are sent'
require_order "$clear_wait_line" "$random_start_line" \
    'random access recovery must be logged after clearing wait state'
require_order "$random_start_line" "$send_nalu_line" \
    'the natural random-access NALU must be sent after cached parameter sets'

if ! grep -Eq '^TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_compactdiag$' "$MAKEFILE"; then
    echo 'small-core build must use the independent compactdiag target' >&2
    exit 1
fi
if ! grep -q 'user/rtsp_sender_withsd_rebase_idrwait' "$MAKEFILE"; then
    echo 'the prior rebase_idrwait binary must remain explicitly preserved' >&2
    exit 1
fi
if ! grep -q 'user/rtsp_sender_withsd_rtprebase' "$MAKEFILE"; then
    echo 'the prior rtprebase binary must remain explicitly preserved' >&2
    exit 1
fi

echo 'PTS rebase natural-IDR wait rules passed'
