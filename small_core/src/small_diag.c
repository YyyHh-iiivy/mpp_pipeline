#include "small_diag.h"

#include <string.h>

void small_diag_tracker_init(small_diag_tracker *tracker, uint64_t now_ms)
{
    if (tracker == NULL) {
        return;
    }

    memset(tracker, 0, sizeof(*tracker));
    tracker->last_valid_ms = now_ms;
}

small_diag_action small_diag_observe(small_diag_tracker *tracker,
                                    uint64_t now_ms,
                                    int has_valid_data,
                                    uint64_t *elapsed_ms)
{
    uint64_t elapsed;

    if (elapsed_ms != NULL) {
        *elapsed_ms = 0;
    }
    if (tracker == NULL) {
        return SMALL_DIAG_NONE;
    }

    if (has_valid_data) {
        if (tracker->stall_active) {
            elapsed = now_ms >= tracker->stall_start_ms ?
                      now_ms - tracker->stall_start_ms : 0;
            tracker->stall_active = 0;
            tracker->last_valid_ms = now_ms;
            tracker->stall_start_ms = 0;
            tracker->last_anomaly_log_ms = 0;
            if (elapsed_ms != NULL) {
                *elapsed_ms = elapsed;
            }
            return SMALL_DIAG_STALL_RECOVERED;
        }

        tracker->last_valid_ms = now_ms;
        return SMALL_DIAG_NONE;
    }

    elapsed = now_ms >= tracker->last_valid_ms ?
              now_ms - tracker->last_valid_ms : 0;
    if (!tracker->stall_active) {
        if (elapsed < SMALL_DIAG_STALL_THRESHOLD_MS) {
            return SMALL_DIAG_NONE;
        }

        tracker->stall_active = 1;
        tracker->stall_start_ms = tracker->last_valid_ms;
        tracker->last_anomaly_log_ms = now_ms;
        if (elapsed_ms != NULL) {
            *elapsed_ms = elapsed;
        }
        return SMALL_DIAG_STALL_START;
    }

    if (now_ms >= tracker->last_anomaly_log_ms &&
        now_ms - tracker->last_anomaly_log_ms >=
        SMALL_DIAG_ANOMALY_INTERVAL_MS) {
        tracker->last_anomaly_log_ms = now_ms;
        if (elapsed_ms != NULL) {
            *elapsed_ms = now_ms - tracker->stall_start_ms;
        }
        return SMALL_DIAG_STALL_ONGOING;
    }

    return SMALL_DIAG_NONE;
}
