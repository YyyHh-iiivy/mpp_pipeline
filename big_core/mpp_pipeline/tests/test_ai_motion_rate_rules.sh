#!/bin/sh
set -eu

root=${1:-.}
header="$root/big_core/mpp_pipeline/mpp_pipeline.h"
motion_file="$root/big_core/mpp_pipeline/motion_thread.c"

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

require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_ACQUIRE_FPS[[:space:]]+5([[:space:]]|$)' \
    "AI motion acquisition FPS must be fixed at 5 for this diagnostic build"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_ACQUIRE_INTERVAL_MS[[:space:]]+200([[:space:]]|$)' \
    "AI motion acquisition interval must be 200ms"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_WAIT_SLICE_MS[[:space:]]+10([[:space:]]|$)' \
    "AI motion wait slices must remain bounded at 10ms"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_HEALTH_INTERVAL_MS[[:space:]]+5000ULL([[:space:]]|$)' \
    "AI health statistics must use a five-second interval"
reject_pattern "$header" \
    'AI_MOTION_PROCESS_(FPS|INTERVAL_MS)' \
    "the obsolete post-dump processing gate must be removed"

require_pattern "$motion_file" \
    'clock_gettime[[:space:]]*\([[:space:]]*CLOCK_MONOTONIC' \
    "AI motion acquisition pacing must use a monotonic clock"
require_pattern "$motion_file" \
    'static k_bool ai_motion_wait_until_next_acquire[[:space:]]*\(' \
    "AI motion pacing must be isolated in a pre-acquire wait helper"
require_pattern "$motion_file" \
    'static void ai_motion_schedule_next_acquire[[:space:]]*\(' \
    "AI motion deadline reset must be isolated in a scheduling helper"

wait_body=$(sed -n '/static k_bool ai_motion_wait_until_next_acquire/,/^}/p' "$motion_file")
schedule_body=$(sed -n '/static void ai_motion_schedule_next_acquire/,/^}/p' "$motion_file")
require_pattern "$motion_file" \
    'AI_MOTION_ACQUIRE_INTERVAL_MS' \
    "AI motion pacing must apply the configured acquisition interval"
if ! printf '%s\n' "$wait_body" | grep -q 'AI_MOTION_WAIT_SLICE_MS'; then
    echo "AI acquisition wait helper must cap sleep to the configured slice"
    exit 1
fi
if printf '%s\n' "$wait_body" | grep -q 'ai_motion_schedule_next_acquire'; then
    echo "AI acquisition wait helper must not advance the deadline before work completes"
    exit 1
fi
if ! printf '%s\n' "$schedule_body" | grep -Eq '\*next_acquire_ms[[:space:]]*=[[:space:]]*ai_motion_now_ms\(\)[[:space:]]*\+[[:space:]]*'; then
    echo "AI acquisition deadlines must restart from current time to prevent catch-up bursts"
    exit 1
fi
if printf '%s\n' "$schedule_body" | grep -Eq '\*next_acquire_ms[[:space:]]*\+='; then
    echo "AI acquisition pacing must not accumulate overdue deadlines"
    exit 1
fi

thread_body=$(sed -n '/static void ai_motion_thread/,/^}/p' "$motion_file")
wait_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_motion_wait_until_next_acquire' | head -n1 | cut -d: -f1)
get_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_frame_try_get(&frame' | head -n1 | cut -d: -f1)
adapter_line=$(printf '%s\n' "$thread_body" | grep -n 'motion_adapter_process(&frame' | head -n1 | cut -d: -f1)
release_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_frame_release(ai_frame_handle)' | head -n1 | cut -d: -f1)

if [ -z "$wait_line" ] || [ -z "$get_line" ] || [ "$wait_line" -ge "$get_line" ]; then
    echo "AI acquisition wait must happen before ai_frame_try_get"
    exit 1
fi
if [ -z "$adapter_line" ] || [ -z "$release_line" ] || \
   [ "$adapter_line" -le "$get_line" ] || [ "$release_line" -le "$adapter_line" ]; then
    echo "every acquired AI frame must be processed and then released"
    exit 1
fi
schedule_after_release=$(printf '%s\n' "$thread_body" | awk '
    /ai_frame_release\(ai_frame_handle\)/ { released=1; next }
    released && /ai_motion_schedule_next_acquire\(&next_acquire_ms\)/ { print; exit }
')
if [ -z "$schedule_after_release" ]; then
    echo "AI acquisition deadline must restart after every acquired frame is released"
    exit 1
fi
schedule_calls=$(printf '%s\n' "$thread_body" | grep -c 'ai_motion_schedule_next_acquire(&next_acquire_ms)' || true)
if [ "$schedule_calls" -ne 2 ]; then
    echo "AI motion thread must reschedule after failed dumps and successful releases only"
    exit 1
fi

held_region=$(printf '%s\n' "$thread_body" | awk '
    /ai_frame_try_get\(&frame/ { holding=1 }
    holding && /ai_frame_release\(ai_frame_handle\)/ { exit }
    holding { print }
')
if printf '%s\n' "$held_region" | grep -q 'rt_thread_mdelay'; then
    echo "AI motion thread must not delay while holding an AI frame"
    exit 1
fi

adapter_calls=$(printf '%s\n' "$thread_body" | grep -c 'motion_adapter_process(&frame' || true)
if [ "$adapter_calls" -ne 1 ]; then
    echo "AI motion thread must process each acquired frame through one adapter call"
    exit 1
fi
reject_pattern "$motion_file" \
    'process_frame|skipped_count[[:space:]]*\+\+' \
    "post-dump frame skipping must be removed"

require_pattern "$motion_file" \
    '\[health:ai\].*dump_fps=.*process_fps=.*dump_ok=.*dump_fail=.*release_fail=.*last_frame_age_ms=' \
    "AI health log must expose acquisition, processing, failure, and freshness fields"
require_pattern "$motion_file" \
    'actual_dump_fps=.*actual_process_fps=' \
    "AI motion exit log must report actual dump and processing FPS"
require_pattern "$motion_file" \
    'AI motion acquire config: dump_fps=%u process_fps=%u interval_ms=%u wait_slice_ms=%u' \
    "AI motion startup log must expose the acquisition fingerprint"

echo "AI motion acquisition rate rules passed"
