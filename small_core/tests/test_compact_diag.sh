#!/bin/sh

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HEADER="$ROOT/lnc/small_diag.h"
MODULE="$ROOT/src/small_diag.c"
SOURCE="$ROOT/src/RTSP_Create.c"
FIFO_SOURCE="$ROOT/src/nalu_datafifo.c"
SNAPSHOT_SOURCE="$ROOT/src/datafifo_snapshot.c"
WRITER_SOURCE="$ROOT/src/snapshot_writer.c"
RTSP_SOURCE="$ROOT/src/rtsp.c"
RTP_SOURCE="$ROOT/src/rtp.c"
MAKEFILE="$ROOT/Makefile"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

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

if [ ! -f "$HEADER" ] || [ ! -f "$MODULE" ]; then
    echo 'small-core compact diagnostic module is missing' >&2
    exit 1
fi

gcc -std=c11 -Wall -Wextra -Werror -I"$ROOT/lnc" \
    "$MODULE" "$ROOT/tests/test_compact_diag.c" \
    -o "$TMPDIR/test_compact_diag"
"$TMPDIR/test_compact_diag"

require_pattern "$HEADER" 'SMALL_DIAG_NORMAL_INTERVAL_MS[[:space:]]+60000ULL' \
    'small-core health summaries must use a 60-second interval'
require_pattern "$HEADER" 'SMALL_DIAG_STALL_THRESHOLD_MS[[:space:]]+500ULL' \
    'small-core no-data stalls must start after 500ms'
require_pattern "$HEADER" 'SMALL_DIAG_ANOMALY_INTERVAL_MS[[:space:]]+1000ULL' \
    'small-core ongoing stalls must use a one-second interval'
require_pattern "$SOURCE" '\[diag\] compact_diag normal=60s stall=500ms anomaly=1s' \
    'small-core compact diagnostic ELF fingerprint is missing'
require_pattern "$SOURCE" '\[health:small\].*uptime_s=.*last_read_seq=.*last_read_done_seq=.*last_sent_seq=.*avail_read=.*idle_ms=.*play=.*wait_random=.*frames=.*rtp_drop=.*outq=.*sndbuf=' \
    'small-core 60-second health summary is missing required fields'
require_pattern "$SOURCE" '\[stall:small\].*state=%s.*cause=%s.*elapsed_ms=.*last_read_seq=.*last_read_done_seq=.*avail_read=.*play=.*wait_random=' \
    'small-core adaptive no-data stall log is missing required fields'
require_pattern "$SOURCE" 'small_diag_observe' \
    'DATAFIFO sender must drive the small-core stall state machine'
monotonic_body=$(sed -n '/^static uint64_t monotonic_ms(void)/,/^}/p' "$SOURCE")
if ! printf '%s\n' "$monotonic_body" | grep -q 'CLOCK_MONOTONIC'; then
    echo 'small-core stall timing must use CLOCK_MONOTONIC' >&2
    exit 1
fi

require_pattern "$SOURCE" 'DATAFIFO_SLOW_COPY_MS[[:space:]]+5ULL' \
    'copy diagnostics must stay quiet at or below 5ms'
require_pattern "$SOURCE" 'DATAFIFO_SLOW_READ_DONE_MS[[:space:]]+5ULL' \
    'READ_DONE diagnostics must stay quiet at or below 5ms'
require_pattern "$SOURCE" 'RTP_SLOW_SEND_MS[[:space:]]+15ULL' \
    'RTP diagnostics must stay quiet at or below 15ms'
require_pattern "$SOURCE" 'RTP_OUTQ_WARN_PERCENT[[:space:]]+75U' \
    'RTP outq diagnostics must trigger at 75 percent of SO_SNDBUF'
require_pattern "$SOURCE" 'RTP_DIAG_ANOMALY_INTERVAL_MS[[:space:]]+1000ULL' \
    'sustained high-outq diagnostics must be limited to once per second'
require_pattern "$RTP_SOURCE" 'RTP_BUSY_LOG_INTERVAL_MS[[:space:]]+1000ULL' \
    'repeated EAGAIN diagnostics must be limited to once per second'
reject_pattern "$SOURCE" 'RTP_DIAG_LOG_INTERVAL_FRAMES' \
    'normal RTP diagnostics must not be emitted every 15 frames'
reject_pattern "$SOURCE" 'DATAFIFO_HEALTH_LOG_INTERVAL_MS[[:space:]]+1000ULL' \
    'normal DATAFIFO health must not be emitted every second'
reject_pattern "$SOURCE" 'reader idle last_seq=' \
    'legacy three-second reader-idle logging must be removed'
reject_pattern "$FIFO_SOURCE" '\[datafifo\] poll idle' \
    'low-level FIFO polling must not duplicate the adaptive stall log'
reject_pattern "$SOURCE" 'frame_count % DATAFIFO_LOG_INTERVAL' \
    'normal per-frame diagnostics must not be frame-count driven'
reject_pattern "$SOURCE" 'no RTP target play=' \
    'frames consumed before PLAY or after disconnect must remain silent'
require_pattern "$SOURCE" 'if \(!force && seq_gap == 0\)' \
    'frame diagnostics must be emitted only for an explicit anomaly or sequence gap'
require_pattern "$SOURCE" 'if \(!force && read_done_ret == 0\)' \
    'normal READ_DONE completion must remain silent'

require_pattern "$SNAPSHOT_SOURCE" '\[snapshot:fail\].*count=.*stage=.*seq=.*ret=.*flags=.*writer=.*frame_irap=.*cached_irap=.*cached_gop=.*params=' \
    'snapshot processing failures must be coalesced into one line'
require_pattern "$SNAPSHOT_SOURCE" 'snapshot_fail_count % 10ULL' \
    'snapshot failure logs must be limited to the first and every tenth failure'
reject_pattern "$SOURCE" '\[snapshot\] copied seq=%llu process ret=' \
    'snapshot processing must not emit a redundant success line'
reject_pattern "$SOURCE" 'snapshot_ret[[:space:]]*!=[[:space:]]*0' \
    'snapshot failures must not force a second per-frame diagnostic line'
reject_pattern "$FIFO_SOURCE" 'NALU_DATAFIFO_VERBOSE_LOG \|\| \(\(\*out_msg\)->reserved' \
    'snapshot-flagged FIFO reads must not emit a low-level success line'
reject_pattern "$SNAPSHOT_SOURCE" '\[snapshot\] copied seq=%llu begin' \
    'copied snapshot processing must not emit a begin line'
reject_pattern "$SNAPSHOT_SOURCE" '\[snapshot\] copied seq=%llu done ret=' \
    'copied snapshot processing must not emit a redundant done line'
reject_pattern "$WRITER_SOURCE" '\[snapshot\] queued len=' \
    'successful snapshot queueing must remain silent'

reject_pattern "$RTSP_SOURCE" '\[rtsp\] request from %s:%d' \
    'RTSP requests, especially OPTIONS keepalives, must not dump the complete request'
require_pattern "$RTSP_SOURCE" 'strcmp\(req.method, "OPTIONS"\)' \
    'OPTIONS keepalive handling must remain active while its full dump is silenced'
require_pattern "$RTSP_SOURCE" 'send_options_response\(client_fd, req.cseq\)' \
    'OPTIONS keepalive response must remain active while request dumps are silenced'

require_pattern "$MAKEFILE" '^TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_compactdiag$' \
    'compact diagnostics must build to an independent ELF name'
require_pattern "$MAKEFILE" 'PRESERVED_REBASE_IDRWAIT_TARGET[[:space:]]*:=[[:space:]]*user/rtsp_sender_withsd_rebase_idrwait' \
    'the prior rebase_idrwait ELF must remain explicitly preserved'
require_pattern "$MAKEFILE" 'src/small_diag\.c' \
    'small_diag.c must be compiled into the small-core ELF'
require_pattern "$MAKEFILE" 'compact_diag normal=60s stall=500ms anomaly=1s' \
    'make verify must check the compact diagnostic version fingerprint'
require_pattern "$MAKEFILE" 'health:small' \
    'make verify must check the small-core health summary fingerprint'
require_pattern "$MAKEFILE" 'stall:small' \
    'make verify must check the small-core stall fingerprint'

echo 'small-core compact diagnostic rules passed'
