#ifndef STREAM_FRESHNESS_H
#define STREAM_FRESHNESS_H

#include "k_type.h"

#define STREAM_FRESHNESS_DROP_THRESHOLD_MS  70LL
#define STREAM_FRESHNESS_CLEAR_THRESHOLD_MS 35LL
#define STREAM_FRESHNESS_LATE_CONFIRM_FRAMES 2U

typedef struct {
    k_bool initialized;
    k_bool catching_up;
    k_u32 late_frame_count;
    k_u64 base_pts_us;
    k_u64 base_now_ms;
    k_s64 max_drift_ms;
    k_u64 stale_drop_count;
} stream_freshness_tracker;

void stream_freshness_init(stream_freshness_tracker *tracker);
k_s32 stream_freshness_observe(stream_freshness_tracker *tracker,
                               k_u64 pts_us,
                               k_u64 now_ms,
                               k_s64 *drift_ms,
                               k_bool *drop);

#endif /* STREAM_FRESHNESS_H */
