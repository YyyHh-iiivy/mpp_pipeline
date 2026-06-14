#include "mpp_pipeline.h"

static k_u32 g_motion_event_id;

static k_u64 motion_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

k_s32 motion_adapter_init(void)
{
    g_motion_event_id = 0;
    LOG("Motion adapter ready");
    return 0;
}

k_s32 motion_adapter_process(const ai_gray_frame_view *frame,
                             motion_event_msg *event,
                             k_bool *has_event)
{
    k_s32 ret;
    motion_detect_result result;

    if (!frame || !event || !has_event)
        return -1;

    memset(event, 0, sizeof(*event));
    memset(&result, 0, sizeof(result));
    *has_event = K_FALSE;

    ret = motion_detect_process(frame, &result);
    if (ret) {
        LOG("motion_detect_process failed! ret=0x%x", ret);
        return ret;
    }

    if (!result.is_motion)
        return 0;

    event->event_id = ++g_motion_event_id;
    event->detect_time_ms = motion_now_ms();
    event->motion_score = result.motion_score;
    event->osd_duration_ms = 1000;
    event->request_snapshot = 0;
    *has_event = K_TRUE;

    return 0;
}

void motion_adapter_deinit(void)
{
    LOG("Motion adapter deinit");
}
