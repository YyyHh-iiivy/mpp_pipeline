#ifndef COMPACT_DIAG_H
#define COMPACT_DIAG_H

#include <stdint.h>

#define COMPACT_DIAG_NORMAL_INTERVAL_MS  60000ULL
#define COMPACT_DIAG_STALL_THRESHOLD_MS    500ULL
#define COMPACT_DIAG_ANOMALY_INTERVAL_MS  1000ULL

typedef enum {
    COMPACT_DIAG_NONE = 0,
    COMPACT_DIAG_STALL_START,
    COMPACT_DIAG_STALL_ONGOING,
    COMPACT_DIAG_STALL_RECOVERED,
} compact_diag_action;

typedef struct {
    uint64_t last_valid_ms;
    uint64_t stall_start_ms;
    uint64_t last_anomaly_log_ms;
    int stall_active;
} compact_diag_tracker;

void compact_diag_tracker_init(compact_diag_tracker *tracker,
                               uint64_t now_ms);

compact_diag_action compact_diag_observe(compact_diag_tracker *tracker,
                                         uint64_t now_ms,
                                         int has_valid_stream,
                                         uint64_t *elapsed_ms);

#endif /* COMPACT_DIAG_H */
