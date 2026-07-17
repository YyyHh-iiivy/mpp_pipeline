#include <string.h>

#include "stream_freshness.h"

static void stream_freshness_set_baseline(stream_freshness_tracker *tracker,
                                          k_u64 pts_us,
                                          k_u64 now_ms)
{
    tracker->initialized = K_TRUE;
    tracker->catching_up = K_FALSE;
    tracker->late_frame_count = 0;
    tracker->base_pts_us = pts_us;
    tracker->base_now_ms = now_ms;
}

void stream_freshness_init(stream_freshness_tracker *tracker)
{
    if (tracker)
        memset(tracker, 0, sizeof(*tracker));
}

k_s32 stream_freshness_observe(stream_freshness_tracker *tracker,
                               k_u64 pts_us,
                               k_u64 now_ms,
                               k_s64 *drift_ms,
                               k_bool *drop)
{
    k_u64 pts_elapsed_ms;
    k_u64 now_elapsed_ms;
    k_s64 current_drift_ms;

    if (!tracker || !drift_ms || !drop)
        return -1;

    *drift_ms = 0;
    *drop = K_FALSE;

    /* Header-only streams use PTS 0 and do not participate in freshness. */
    if (pts_us == 0)
        return 0;

    if (!tracker->initialized ||
        pts_us < tracker->base_pts_us ||
        now_ms < tracker->base_now_ms) {
        stream_freshness_set_baseline(tracker, pts_us, now_ms);
        return 0;
    }

    pts_elapsed_ms = (pts_us - tracker->base_pts_us) / 1000ULL;
    now_elapsed_ms = now_ms - tracker->base_now_ms;
    current_drift_ms = (k_s64)now_elapsed_ms - (k_s64)pts_elapsed_ms;
    *drift_ms = current_drift_ms;

    if (current_drift_ms > tracker->max_drift_ms)
        tracker->max_drift_ms = current_drift_ms;

    if (tracker->catching_up) {
        if (current_drift_ms <= STREAM_FRESHNESS_CLEAR_THRESHOLD_MS) {
            tracker->catching_up = K_FALSE;
            tracker->late_frame_count = 0;
            return 0;
        }

        tracker->stale_drop_count++;
        *drop = K_TRUE;
        return 0;
    }

    if (current_drift_ms > STREAM_FRESHNESS_DROP_THRESHOLD_MS) {
        if (tracker->late_frame_count < STREAM_FRESHNESS_LATE_CONFIRM_FRAMES)
            tracker->late_frame_count++;
    } else {
        tracker->late_frame_count = 0;
    }

    if (tracker->late_frame_count >= STREAM_FRESHNESS_LATE_CONFIRM_FRAMES) {
        tracker->catching_up = K_TRUE;
        tracker->stale_drop_count++;
        *drop = K_TRUE;
    }

    return 0;
}
