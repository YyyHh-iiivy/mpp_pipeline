#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "rtp_clock.h"

int main(void)
{
    const uint32_t base_ts = 0x12345678U;
    rtp_clock_state_t clock_state;
    uint64_t pts_delta_us = 0;
    uint32_t rtp_delta = 0;
    uint32_t ts1;
    uint32_t ts2;
    uint32_t ts3;
    uint32_t ts4;
    uint32_t ts5;
    uint32_t ts6;
    uint32_t ts7;
    uint32_t ts8;
    uint32_t ts9;
    int pts_mode = 0;

    rtp_clock_reset(&clock_state, base_ts);

    ts1 = rtp_clock_next_timestamp(&clock_state,
                                   0,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts1 == base_ts + RTP_TS_STEP);
    assert(pts_mode == 0);
    assert(rtp_delta == RTP_TS_STEP);

    ts2 = rtp_clock_next_timestamp(&clock_state,
                                   1000000ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts2 > ts1);
    assert(ts2 - ts1 == RTP_TS_STEP);
    assert(pts_mode == 1);
    assert(rtp_delta == RTP_TS_STEP);

    ts3 = rtp_clock_next_timestamp(&clock_state,
                                   1066696ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts3 > ts2);
    assert(pts_delta_us == 66696ULL);
    assert(rtp_delta >= 6002U && rtp_delta <= 6003U);

    ts4 = rtp_clock_next_timestamp(&clock_state,
                                   1200087ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts4 > ts3);
    assert(pts_delta_us == 133391ULL);
    assert(rtp_delta >= 12005U && rtp_delta <= 12006U);

    ts5 = rtp_clock_next_timestamp(&clock_state,
                                   0,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts5 == ts4 + RTP_TS_STEP);
    assert(pts_mode == 0);
    assert(rtp_delta == RTP_TS_STEP);

    ts6 = rtp_clock_next_timestamp(&clock_state,
                                   3500000ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts6 - ts5 ==
           (uint32_t)(((3500000ULL - 1200087ULL) * RTP_CLOCK_RATE) /
                      1000000ULL));
    assert(pts_mode == 1);
    assert(pts_delta_us == 3500000ULL - 1200087ULL);
    assert(rtp_delta == ts6 - ts5);
    assert(clock_state.rebase_count == 1);
    assert(clock_state.rebase_from_pts_us == 1200087ULL);
    assert(clock_state.rebase_to_pts_us == 3500000ULL);

    ts7 = rtp_clock_next_timestamp(&clock_state,
                                   3566696ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts7 > ts6);
    assert(pts_mode == 1);
    assert(pts_delta_us == 66696ULL);

    ts8 = rtp_clock_next_timestamp(&clock_state,
                                   500000ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts8 == ts7 + RTP_TS_STEP);
    assert(pts_mode == 0);
    assert(pts_delta_us == 0);
    assert(clock_state.rebase_count == 2);
    assert(clock_state.rebase_from_pts_us == 3566696ULL);
    assert(clock_state.rebase_to_pts_us == 500000ULL);

    ts9 = rtp_clock_next_timestamp(&clock_state,
                                   566696ULL,
                                   &pts_mode,
                                   &pts_delta_us,
                                   &rtp_delta);
    assert(ts9 > ts8);
    assert(pts_mode == 1);
    assert(pts_delta_us == 66696ULL);

    puts("RTP clock tests passed");
    return 0;
}
