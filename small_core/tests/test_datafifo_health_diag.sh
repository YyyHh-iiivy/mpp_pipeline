#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE="$ROOT/src/RTSP_Create.c"
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

require_pattern "$SOURCE" '\[health:small\].*last_read_seq=.*last_read_done_seq=' \
    'DATAFIFO health heartbeat is missing read/read_done progress'
require_pattern "$SOURCE" 'avail_read=.*idle_ms=.*play=.*wait_random=' \
    'DATAFIFO health heartbeat is missing FIFO and playback state'
require_pattern "$SOURCE" '\[datafifo:large\].*stage=%s.*ret=.*cost_ms=' \
    'Large-frame slow/failure checkpoint is missing'
require_pattern "$SOURCE" '\[rtp:rebase\].*count=.*from_pts=.*to_pts=' \
    'RTP PTS discontinuity rebase diagnostic is missing'
require_pattern "$MAKEFILE" 'rtsp_sender_withsd_stabilitydiag' \
    'The stability diagnostic ELF must have an independent output name'
require_pattern "$MAKEFILE" 'PRESERVED_REBASE_IDRWAIT_TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_rebase_idrwait' \
    'The rebase IDR-wait ELF must remain preserved'
require_pattern "$MAKEFILE" 'PRESERVED_RTPREBASE_TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_rtprebase' \
    'The prior RTP rebase ELF must remain preserved'

echo 'DATAFIFO health diagnostic rules passed'
