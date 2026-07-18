#include "compact_diag.h"

#include <stddef.h>
#include <string.h>

void compact_diag_tracker_init(compact_diag_tracker *tracker,
                               uint64_t now_ms)
{
    if (!tracker)
        return;

    memset(tracker, 0, sizeof(*tracker));
    tracker->last_valid_ms = now_ms;
}

compact_diag_action compact_diag_observe(compact_diag_tracker *tracker,
                                         uint64_t now_ms,
                                         int has_valid_stream,
                                         uint64_t *elapsed_ms)
{
    uint64_t elapsed;

    if (elapsed_ms)
        *elapsed_ms = 0;
    if (!tracker)
        return COMPACT_DIAG_NONE;

    if (has_valid_stream) {
        if (tracker->stall_active) {
            elapsed = now_ms >= tracker->stall_start_ms ?
                      now_ms - tracker->stall_start_ms : 0;
            tracker->stall_active = 0;
            tracker->last_valid_ms = now_ms;
            tracker->stall_start_ms = 0;
            tracker->last_anomaly_log_ms = 0;
            if (elapsed_ms)
                *elapsed_ms = elapsed;
            return COMPACT_DIAG_STALL_RECOVERED;
        }

        tracker->last_valid_ms = now_ms;
        return COMPACT_DIAG_NONE;
    }

    elapsed = now_ms >= tracker->last_valid_ms ?
              now_ms - tracker->last_valid_ms : 0;
    if (!tracker->stall_active) {
        if (elapsed < COMPACT_DIAG_STALL_THRESHOLD_MS)
            return COMPACT_DIAG_NONE;

        tracker->stall_active = 1;
        tracker->stall_start_ms = tracker->last_valid_ms;
        tracker->last_anomaly_log_ms = now_ms;
        if (elapsed_ms)
            *elapsed_ms = elapsed;
        return COMPACT_DIAG_STALL_START;
    }

    if (now_ms >= tracker->last_anomaly_log_ms &&
        now_ms - tracker->last_anomaly_log_ms >=
        COMPACT_DIAG_ANOMALY_INTERVAL_MS) {
        tracker->last_anomaly_log_ms = now_ms;
        if (elapsed_ms)
            *elapsed_ms = now_ms - tracker->stall_start_ms;
        return COMPACT_DIAG_STALL_ONGOING;
    }

    return COMPACT_DIAG_NONE;
}
