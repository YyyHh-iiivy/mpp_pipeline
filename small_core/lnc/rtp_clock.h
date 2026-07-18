#ifndef RTP_CLOCK_H
#define RTP_CLOCK_H

#include <stdint.h>

#define RTP_CLOCK_RATE 90000U
#define VIDEO_FPS      15U
#define RTP_TS_STEP    (RTP_CLOCK_RATE / VIDEO_FPS)

typedef struct {
    int valid;
    uint64_t base_pts_us;
    uint64_t last_pts_us;
    uint32_t base_rtp_ts;
    uint32_t last_rtp_ts;
    uint64_t rebase_count;
    uint64_t rebase_from_pts_us;
    uint64_t rebase_to_pts_us;
} rtp_clock_state_t;

void rtp_clock_reset(rtp_clock_state_t *clock_state, uint32_t base_ts);

uint32_t rtp_clock_next_timestamp(rtp_clock_state_t *clock_state,
                                  uint64_t frame_pts_us,
                                  int *pts_mode,
                                  uint64_t *pts_delta_us,
                                  uint32_t *rtp_delta);

#endif /* RTP_CLOCK_H */
