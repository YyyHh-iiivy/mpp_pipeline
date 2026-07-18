#ifndef SMALL_DIAG_H
#define SMALL_DIAG_H

#include <stdint.h>

#define SMALL_DIAG_NORMAL_INTERVAL_MS  60000ULL
#define SMALL_DIAG_STALL_THRESHOLD_MS    500ULL
#define SMALL_DIAG_ANOMALY_INTERVAL_MS  1000ULL

typedef enum {
    SMALL_DIAG_NONE = 0,
    SMALL_DIAG_STALL_START,
    SMALL_DIAG_STALL_ONGOING,
    SMALL_DIAG_STALL_RECOVERED,
} small_diag_action;

typedef struct {
    uint64_t last_valid_ms;
    uint64_t stall_start_ms;
    uint64_t last_anomaly_log_ms;
    int stall_active;
} small_diag_tracker;

void small_diag_tracker_init(small_diag_tracker *tracker, uint64_t now_ms);

small_diag_action small_diag_observe(small_diag_tracker *tracker,
                                    uint64_t now_ms,
                                    int has_valid_data,
                                    uint64_t *elapsed_ms);

#endif /* SMALL_DIAG_H */
