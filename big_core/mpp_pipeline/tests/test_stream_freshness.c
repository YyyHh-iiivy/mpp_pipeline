#include <stdio.h>

#include "stream_freshness.h"

static int g_failures;

#define EXPECT_TRUE(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL: %s\n", message); \
            g_failures++; \
        } \
    } while (0)

static void observe(stream_freshness_tracker *tracker,
                    k_u64 pts_us,
                    k_u64 now_ms,
                    k_s64 expected_drift_ms,
                    k_bool expected_drop,
                    const char *message)
{
    k_s64 drift_ms = -9999;
    k_bool drop = K_FALSE;
    k_s32 ret;

    ret = stream_freshness_observe(tracker, pts_us, now_ms,
                                   &drift_ms, &drop);
    EXPECT_TRUE(ret == 0, message);
    EXPECT_TRUE(drift_ms == expected_drift_ms, message);
    EXPECT_TRUE(drop == expected_drop, message);
}

int main(void)
{
    stream_freshness_tracker tracker;

    stream_freshness_init(&tracker);
    observe(&tracker, 0, 1000, 0, K_FALSE,
            "header PTS must not initialize freshness tracking");
    EXPECT_TRUE(tracker.initialized == K_FALSE,
                "zero PTS must leave the tracker uninitialized");

    observe(&tracker, 1000000, 1000, 0, K_FALSE,
            "first video frame must establish the baseline");
    observe(&tracker, 1066667, 1067, 1, K_FALSE,
            "normal frame cadence must not be dropped");

    observe(&tracker, 1133334, 1210, 77, K_FALSE,
            "first late frame must only arm the confirmation counter");
    observe(&tracker, 1200001, 1280, 80, K_TRUE,
            "second consecutive late frame must enter catch-up mode");
    observe(&tracker, 1266668, 1317, 51, K_TRUE,
            "catch-up mode must keep dropping above the clear threshold");
    observe(&tracker, 1333335, 1368, 35, K_FALSE,
            "catch-up mode must stop at the 35ms clear threshold");

    EXPECT_TRUE(tracker.stale_drop_count == 2,
                "tracker must count every dropped stale frame");
    EXPECT_TRUE(tracker.max_drift_ms == 80,
                "tracker must retain the maximum observed drift");
    EXPECT_TRUE(tracker.catching_up == K_FALSE,
                "tracker must leave catch-up mode after recovery");

    observe(&tracker, 900000, 1400, 0, K_FALSE,
            "PTS regression must reset the freshness baseline safely");

    if (g_failures) {
        printf("%d stream freshness test(s) failed\n", g_failures);
        return 1;
    }

    printf("stream freshness tests passed\n");
    return 0;
}
