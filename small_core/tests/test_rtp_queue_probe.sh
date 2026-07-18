#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RTP_HEADER="$ROOT/lnc/rtp.h"
RTP_SOURCE="$ROOT/src/rtp.c"
MAIN_SOURCE="$ROOT/src/RTSP_Create.c"
MAKEFILE="$ROOT/Makefile"

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

require_pattern "$RTP_HEADER" 'rtp_get_send_queue_bytes' \
    'RTP queue probe API is missing'
require_pattern "$RTP_HEADER" 'rtp_get_actual_send_buffer' \
    'RTP actual SO_SNDBUF probe API is missing'
require_pattern "$RTP_SOURCE" 'SIOCOUTQ|TIOCOUTQ' \
    'RTP queue probe must use the Linux socket output-queue ioctl'
require_pattern "$RTP_SOURCE" 'getsockopt.*SO_SNDBUF' \
    'RTP queue probe must read the actual kernel SO_SNDBUF value'
require_pattern "$MAIN_SOURCE" '\[rtp:diag\].*outq_before=.*outq_after=.*outq_high=' \
    'RTP per-frame queue diagnostic log is missing'
require_pattern "$MAIN_SOURCE" 'rtp_step=.*rtp_elapsed_ms=.*wall_elapsed_ms=.*clock_drift_ms=' \
    'RTP clock drift diagnostic fields are missing'
require_pattern "$MAIN_SOURCE" 'RTP_OUTQ_WARN_PERCENT[[:space:]]+75U' \
    'RTP queue diagnostics must trigger when outq reaches 75 percent of SO_SNDBUF'
require_pattern "$MAKEFILE" 'rtsp_sender_withsd_queueprobe' \
    'The queue-probe ELF must have an independent output name'

require_pattern "$RTP_HEADER" '^#define RTP_SOCKET_SNDBUF[[:space:]]+\(128 \* 1024\)' \
    'The diagnostic build must not change SO_SNDBUF'
require_pattern "$RTP_HEADER" '^#define RTP_PACKET_PACE_US[[:space:]]+0' \
    'The diagnostic build must not enable RTP pacing'

echo 'RTP queue probe rules passed'
