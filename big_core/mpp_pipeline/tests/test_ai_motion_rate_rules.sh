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
        echo "$message" >&2
        exit 1
    fi
}

reject_pattern() {
    file=$1
    pattern=$2
    message=$3

    if grep -Eq "$pattern" "$file"; then
        echo "$message" >&2
        exit 1
    fi
}

require_pattern "$header" \
    '^[[:space:]]*#ifndef[[:space:]]+AI_MOTION_ACQUIRE_FPS' \
    "AI motion acquisition FPS must be overridable"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_ACQUIRE_FPS[[:space:]]+5([[:space:]]|$)' \
    "AI motion acquisition FPS must be 5"
require_pattern "$header" \
    'AI_MOTION_ACQUIRE_INTERVAL_MS[[:space:]]+\(1000U[[:space:]]*/[[:space:]]*AI_MOTION_ACQUIRE_FPS\)' \
    "AI motion acquisition interval must derive from FPS"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_WAIT_SLICE_MS[[:space:]]+10([[:space:]]|$)' \
    "AI motion wait slices must be 10ms"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_HEALTH_INTERVAL_MS[[:space:]]+5000ULL([[:space:]]|$)' \
    "AI health statistics must use a five-second interval"
reject_pattern "$header" \
    'AI_MOTION_PROCESS_(FPS|INTERVAL_MS)' \
    "obsolete post-dump processing gate must not be present"

require_pattern "$motion_file" \
    'clock_gettime[[:space:]]*\([[:space:]]*CLOCK_MONOTONIC' \
    "AI pacing must use a monotonic clock"
require_pattern "$motion_file" \
    'static k_bool ai_motion_wait_until_next_acquire[[:space:]]*\(' \
    "AI wait must be isolated in a helper"
require_pattern "$motion_file" \
    'static void ai_motion_schedule_next_acquire[[:space:]]*\(' \
    "AI scheduling must be isolated in a helper"

wait_body=$(sed -n '/static k_bool ai_motion_wait_until_next_acquire/,/^}/p' "$motion_file")
schedule_body=$(sed -n '/static void ai_motion_schedule_next_acquire/,/^}/p' "$motion_file")
if ! printf '%s\n' "$wait_body" | grep -q 'AI_MOTION_WAIT_SLICE_MS'; then
    echo "AI wait helper must cap sleep to the configured slice" >&2
    exit 1
fi
if printf '%s\n' "$wait_body" | grep -q 'ai_motion_schedule_next_acquire'; then
    echo "AI wait helper must not advance the deadline" >&2
    exit 1
fi
if ! printf '%s\n' "$schedule_body" | grep -Eq '\*next_acquire_ms[[:space:]]*=[[:space:]]*ai_motion_now_ms\(\)[[:space:]]*\+'; then
    echo "AI deadlines must restart from current time" >&2
    exit 1
fi
if printf '%s\n' "$schedule_body" | grep -Eq '\*next_acquire_ms[[:space:]]*\+='; then
    echo "AI pacing must not accumulate overdue deadlines" >&2
    exit 1
fi

thread_body=$(sed -n '/static void ai_motion_thread/,/^}/p' "$motion_file")
wait_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_motion_wait_until_next_acquire' | head -n1 | cut -d: -f1)
get_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_frame_try_get(&frame' | head -n1 | cut -d: -f1)
adapter_line=$(printf '%s\n' "$thread_body" | grep -n 'motion_adapter_process(&frame' | head -n1 | cut -d: -f1)
release_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_frame_release(ai_frame_handle)' | head -n1 | cut -d: -f1)

if [ -z "$wait_line" ] || [ -z "$get_line" ] || [ "$wait_line" -ge "$get_line" ]; then
    echo "AI wait must happen before frame acquisition" >&2
    exit 1
fi
if [ -z "$adapter_line" ] || [ -z "$release_line" ] || \
   [ "$adapter_line" -le "$get_line" ] || [ "$release_line" -le "$adapter_line" ]; then
    echo "every acquired AI frame must be processed then released" >&2
    exit 1
fi

if ! printf '%s\n' "$thread_body" | grep -q 'ai_motion_schedule_next_acquire(&next_acquire_ms)'; then
    echo "AI thread must reschedule after dump attempts" >&2
    exit 1
fi

held_region=$(printf '%s\n' "$thread_body" | awk '
    /ai_frame_try_get\(&frame/ { holding=1 }
    holding && /ai_frame_release\(ai_frame_handle\)/ { exit }
    holding { print }
')
if printf '%s\n' "$held_region" | grep -q 'rt_thread_mdelay'; then
    echo "AI motion thread must not delay while holding an AI frame" >&2
    exit 1
fi

require_pattern "$motion_file" \
    '\[health:ai\].*dump_fps=.*process_fps=.*dump_ok=.*dump_fail=.*release_fail=.*last_frame_age_ms=' \
    "AI health log must expose rate and freshness fields"
require_pattern "$motion_file" \
    'actual_dump_fps=.*actual_process_fps=' \
    "AI exit log must report actual FPS"

echo "AI motion acquisition rate rules passed"
