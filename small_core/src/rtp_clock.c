#include "rtp_clock.h"

#include <stddef.h>
#include <string.h>

void rtp_clock_reset(rtp_clock_state_t *clock_state, uint32_t base_ts)
{
    if (clock_state == NULL) {
        return;
    }

    memset(clock_state, 0, sizeof(*clock_state));
    clock_state->base_rtp_ts = base_ts;
    clock_state->last_rtp_ts = base_ts;
}

uint32_t rtp_clock_next_timestamp(rtp_clock_state_t *clock_state,
                                  uint64_t frame_pts_us,
                                  int *pts_mode,
                                  uint64_t *pts_delta_us,
                                  uint32_t *rtp_delta)
{
    uint32_t next_ts;
    int use_pts = 0;

    if (pts_mode != NULL) {
        *pts_mode = 0;
    }
    if (pts_delta_us != NULL) {
        *pts_delta_us = 0;
    }
    if (rtp_delta != NULL) {
        *rtp_delta = 0;
    }

    if (clock_state == NULL) {
        return 0x12345678U;
    }

    next_ts = clock_state->last_rtp_ts + RTP_TS_STEP;
    if (frame_pts_us != 0) {
        if (!clock_state->valid) {
            clock_state->valid = 1;
            clock_state->base_pts_us = frame_pts_us;
            clock_state->last_pts_us = frame_pts_us;
            clock_state->base_rtp_ts = next_ts;
            use_pts = 1;
        } else if (frame_pts_us > clock_state->last_pts_us &&
                   frame_pts_us - clock_state->last_pts_us <= 1000000ULL) {
            uint64_t rel_us = frame_pts_us - clock_state->base_pts_us;
            uint64_t delta_us = frame_pts_us - clock_state->last_pts_us;

            next_ts = clock_state->base_rtp_ts +
                      (uint32_t)((rel_us * RTP_CLOCK_RATE) / 1000000ULL);
            if (next_ts <= clock_state->last_rtp_ts) {
                next_ts = clock_state->last_rtp_ts + RTP_TS_STEP;
            }
            if (pts_delta_us != NULL) {
                *pts_delta_us = delta_us;
            }
            use_pts = 1;
        } else if (frame_pts_us > clock_state->last_pts_us) {
            uint64_t delta_us = frame_pts_us - clock_state->last_pts_us;

            next_ts = clock_state->last_rtp_ts +
                      (uint32_t)((delta_us * RTP_CLOCK_RATE) / 1000000ULL);
            clock_state->rebase_count++;
            clock_state->rebase_from_pts_us = clock_state->last_pts_us;
            clock_state->rebase_to_pts_us = frame_pts_us;
            clock_state->base_pts_us = frame_pts_us;
            clock_state->last_pts_us = frame_pts_us;
            clock_state->base_rtp_ts = next_ts;
            if (pts_delta_us != NULL) {
                *pts_delta_us = delta_us;
            }
            use_pts = 1;
        } else {
            /*
             * Keep a rollback/duplicate frame close to the previous RTP
             * timestamp, then accept its PTS as the next-frame baseline.
             */
            clock_state->rebase_count++;
            clock_state->rebase_from_pts_us = clock_state->last_pts_us;
            clock_state->rebase_to_pts_us = frame_pts_us;
            clock_state->base_pts_us = frame_pts_us;
            clock_state->last_pts_us = frame_pts_us;
            clock_state->base_rtp_ts = next_ts;
        }
    }

    if (rtp_delta != NULL) {
        *rtp_delta = next_ts - clock_state->last_rtp_ts;
    }
    if (pts_mode != NULL) {
        *pts_mode = use_pts;
    }

    if (use_pts && frame_pts_us != 0) {
        clock_state->last_pts_us = frame_pts_us;
    }
    clock_state->last_rtp_ts = next_ts;
    return next_ts;
}
