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

require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_PROCESS_FPS[[:space:]]+15([[:space:]]|$)' \
    "AI motion processing FPS must be fixed at 15"
require_pattern "$header" \
    '^[[:space:]]*#define[[:space:]]+AI_MOTION_PROCESS_INTERVAL_MS[[:space:]]+66([[:space:]]|$)' \
    "AI motion processing interval must be 66ms"
require_pattern "$motion_file" \
    'clock_gettime[[:space:]]*\([[:space:]]*CLOCK_MONOTONIC' \
    "AI motion rate limiting must use a monotonic clock"
require_pattern "$motion_file" \
    'AI_MOTION_PROCESS_INTERVAL_MS' \
    "AI motion thread must apply the configured processing interval"
require_pattern "$motion_file" \
    'motion_adapter_process[[:space:]]*\(' \
    "AI motion thread must retain the motion adapter call"
require_pattern "$motion_file" \
    'dump_count|frame_count' \
    "AI motion thread must count dumped frames"
require_pattern "$motion_file" \
    'processed_count|process_count' \
    "AI motion thread must count processed frames"
require_pattern "$motion_file" \
    'skipped_count|skip_count' \
    "AI motion thread must count skipped frames"
require_pattern "$motion_file" \
    'actual.*fps|fps=.*processed|processed.*fps' \
    "AI motion thread exit log must report actual processing FPS"
require_pattern "$motion_file" \
    'AI motion processing config: fps=%u interval_ms=%u' \
    "AI motion startup log must expose the processing fingerprint"

thread_body=$(sed -n '/static void ai_motion_thread/,/^}/p' "$motion_file")
release_line=$(printf '%s\n' "$thread_body" | grep -n 'ai_frame_release(ai_frame_handle)' | head -n1 | cut -d: -f1)
adapter_line=$(printf '%s\n' "$thread_body" | grep -n 'motion_adapter_process(&frame' | head -n1 | cut -d: -f1)
if [ -z "$release_line" ] || [ -z "$adapter_line" ] || [ "$release_line" -le "$adapter_line" ]; then
    echo "every dumped AI frame must be released after the optional motion processing"
    exit 1
fi

before_release=$(printf '%s\n' "$thread_body" | awk '/dump_count\+\+|frame_count\+\+/{ started=1 } started && /ai_frame_release\(ai_frame_handle\)/{ exit } started { print }')
if printf '%s\n' "$before_release" | grep -Eq 'rt_thread_mdelay'; then
    echo "AI motion thread must not delay while holding an AI frame"
    exit 1
fi

if ! printf '%s\n' "$thread_body" | grep -Eq 'if[[:space:]]*\([^)]*(process|due|interval)[^)]*\)[[:space:]]*\{'; then
    echo "AI motion algorithm call must be guarded by a processing-period check"
    exit 1
fi

process_branch=$(printf '%s\n' "$thread_body" | awk '
    /if[[:space:]]*\(process_frame\)/ { inside=1 }
    inside { print }
    inside && /}[[:space:]]*else[[:space:]]*{/ { exit }
')
if ! printf '%s\n' "$process_branch" | grep -q 'motion_adapter_process(&frame'; then
    echo "motion_adapter_process must remain inside the process-frame branch"
    exit 1
fi

skip_branch=$(printf '%s\n' "$thread_body" | awk '
    /}[[:space:]]*else[[:space:]]*{/ { inside=1 }
    inside { print }
    inside && /skipped_count\+\+/ { seen_skip=1 }
    seen_skip && /^        }/ { print; exit }
')
if printf '%s\n' "$skip_branch" | grep -q 'motion_adapter_process(&frame'; then
    echo "skipped AI frames must not call motion_adapter_process"
    exit 1
fi

echo "AI motion processing rate rules passed"
