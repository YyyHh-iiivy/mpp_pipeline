#!/bin/sh
set -eu

root=${1:-.}
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/mpp_pipeline.h" <<'EOF'
#ifndef MPP_PIPELINE_H
#define MPP_PIPELINE_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "k_type.h"
#include "mpp_types.h"
#include "motion_detect.h"

#define LOG(fmt, ...) printf("[MPP] " fmt "\n", ##__VA_ARGS__)

k_s32 motion_adapter_init(void);
k_s32 motion_adapter_process(const ai_gray_frame_view *frame,
                             motion_event_msg *event,
                             k_bool *has_event);
void motion_adapter_deinit(void);

#endif /* MPP_PIPELINE_H */
EOF

cat > "$tmpdir/test_motion_adapter_debounce.c" <<'EOF'
#include "mpp_pipeline.h"

static int g_failures;
static motion_detect_result g_results[16];
static unsigned int g_result_count;
static unsigned int g_result_index;
static k_u64 g_now_ms;

#define EXPECT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            g_failures++; \
        } \
    } while (0)

static void set_result(unsigned int index, k_u32 is_motion, k_u32 score)
{
    g_results[index].is_motion = is_motion;
    g_results[index].motion_score = score;
}

int test_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    (void)clock_id;

    ts->tv_sec = (time_t)(g_now_ms / 1000ULL);
    ts->tv_nsec = (long)((g_now_ms % 1000ULL) * 1000000ULL);
    return 0;
}

static void set_now_ms(k_u64 now_ms)
{
    g_now_ms = now_ms;
}

k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result)
{
    (void)frame;

    if (!result || g_result_index >= g_result_count)
        return -1;

    *result = g_results[g_result_index++];
    return 0;
}

static void run_one(const ai_gray_frame_view *frame,
                    k_bool expected_has_event,
                    const char *message)
{
    motion_event_msg event;
    k_bool has_event = K_FALSE;
    k_s32 ret;

    ret = motion_adapter_process(frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, message);
    EXPECT_TRUE(has_event == expected_has_event, message);
}

int main(void)
{
    k_u8 dummy_luma = 0;
    ai_gray_frame_view frame;
    motion_event_msg event;
    k_bool has_event;
    k_s32 ret;
    unsigned int i;

    memset(&frame, 0, sizeof(frame));
    frame.width = 1;
    frame.height = 1;
    frame.stride = 1;
    frame.y = &dummy_luma;

    for (i = 0; i < 5; ++i)
        set_result(i, K_TRUE, 42);
    for (i = 5; i < 10; ++i)
        set_result(i, K_FALSE, 0);
    for (i = 10; i < 13; ++i)
        set_result(i, K_TRUE, 88);
    g_result_count = 13;

    ret = motion_adapter_init();
    EXPECT_TRUE(ret == 0, "motion_adapter_init should succeed");

    set_now_ms(1000);
    run_one(&frame, K_FALSE, "first motion frame should not trigger event");
    run_one(&frame, K_FALSE, "second motion frame should not trigger event");

    memset(&event, 0, sizeof(event));
    has_event = K_FALSE;
    ret = motion_adapter_process(&frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, "third motion frame should be accepted");
    EXPECT_TRUE(has_event == K_TRUE, "third consecutive motion frame should trigger event");
    EXPECT_TRUE(event.event_id == 1, "first event id should be 1");
    EXPECT_TRUE(event.motion_score == 42, "event should carry detector score");
    EXPECT_TRUE(event.osd_duration_ms == 1000, "event should keep 1000ms OSD duration");
    EXPECT_TRUE(event.request_snapshot == 1, "motion event should request snapshot");

    set_now_ms(4999);
    run_one(&frame, K_FALSE, "continued motion before 4000ms should not retrigger");

    set_now_ms(5000);
    memset(&event, 0, sizeof(event));
    has_event = K_FALSE;
    ret = motion_adapter_process(&frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, "continued motion at 4000ms should be accepted");
    EXPECT_TRUE(has_event == K_TRUE,
                "continued motion at 4000ms should trigger periodic event");
    EXPECT_TRUE(event.event_id == 2, "periodic event id should be 2");
    EXPECT_TRUE(event.motion_score == 42,
                "periodic event should carry the current detector score");
    EXPECT_TRUE(event.osd_duration_ms == 1000,
                "periodic event should keep 1000ms OSD duration");
    EXPECT_TRUE(event.request_snapshot == 1,
                "periodic motion event should request snapshot");

    for (i = 0; i < 5; ++i)
        run_one(&frame, K_FALSE, "still frames should end motion segment without event");

    set_now_ms(5100);
    run_one(&frame, K_FALSE, "new segment first motion frame should not trigger");
    run_one(&frame, K_FALSE, "new segment second motion frame should not trigger");

    memset(&event, 0, sizeof(event));
    has_event = K_FALSE;
    ret = motion_adapter_process(&frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, "new segment third motion frame should be accepted");
    EXPECT_TRUE(has_event == K_TRUE,
                "new segment should trigger without waiting for old 4000ms period");
    EXPECT_TRUE(event.event_id == 3, "new segment event id should be 3");

    motion_adapter_deinit();

    if (g_failures) {
        printf("%d motion adapter debounce test(s) failed\n", g_failures);
        return 1;
    }

    printf("motion adapter debounce tests passed\n");
    return 0;
}
EOF

cp "$root/big_core/mpp_pipeline/motion_adapter.c" "$tmpdir/motion_adapter.c"

gcc -Wall -Wextra \
    -DMOTION_STATE_DIAG_ENABLE=1 \
    -Dclock_gettime=test_clock_gettime \
    -I"$tmpdir" \
    -I"$root/big_core/mpp_pipeline" \
    -I/home/ubuntu/k230_sdk/src/big/mpp/include \
    -c "$tmpdir/motion_adapter.c" \
    -o "$tmpdir/motion_adapter.o"

gcc -Wall -Wextra \
    -DMOTION_STATE_DIAG_ENABLE=1 \
    -I"$tmpdir" \
    -I"$root/big_core/mpp_pipeline" \
    -I/home/ubuntu/k230_sdk/src/big/mpp/include \
    -c "$tmpdir/test_motion_adapter_debounce.c" \
    -o "$tmpdir/test_motion_adapter_debounce.o"

gcc \
    "$tmpdir/motion_adapter.o" \
    "$tmpdir/test_motion_adapter_debounce.o" \
    -o "$tmpdir/test_motion_adapter_debounce"

output=$("$tmpdir/test_motion_adapter_debounce")
printf '%s\n' "$output"

require_state()
{
    pattern=$1
    message=$2

    if ! printf '%s\n' "$output" | grep -Eq "$pattern"; then
        echo "FAIL: $message" >&2
        exit 1
    fi
}

require_state '\[motion:state\].*is_motion=1.*hit=1.*clear=0.*active=0.*reason=confirm_wait' \
    "first hit must wait for confirmation"
require_state '\[motion:state\].*is_motion=1.*hit=3.*clear=0.*active=1.*reason=event' \
    "third hit must report an event"
require_state '\[motion:state\].*is_motion=1.*hit=3.*active=1.*reason=period_wait' \
    "continued motion before 4000ms must report periodic wait"
require_state '\[motion:state\].*is_motion=0.*hit=0.*clear=1.*active=1.*reason=clear_wait' \
    "first still frame must wait for motion segment end"
require_state '\[motion:state\].*is_motion=0.*hit=0.*clear=5.*active=0.*reason=no_motion' \
    "fifth still frame must report the ended motion segment"
