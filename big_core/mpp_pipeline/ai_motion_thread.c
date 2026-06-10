#include "mpp_pipeline.h"

static pthread_t g_ai_motion_tid;
static volatile int g_ai_motion_running;
static k_bool g_ai_motion_started;

static void *ai_motion_thread(void *arg)
{
    k_u32 timeout_count = 0;
    k_u32 frame_count = 0;

    (void)arg;
    LOG("AI motion thread started");

    while (g_ai_motion_running && g_running) {
        k_s32 ret;
        ai_gray_frame_view frame;
        motion_event_msg event;
        k_bool has_event = K_FALSE;
        void *handle = NULL;

        ret = ai_frame_try_get(&frame, &handle);
        if (ret) {
            if ((timeout_count++ % 100) == 0)
                LOG("AI frame dump timeout/no frame ret=0x%x", ret);
            usleep(10000);
            continue;
        }
        timeout_count = 0;
        frame_count++;

        if (frame_count == 1) {
            LOG("AI frame first dump: chn=%d size=%ux%u stride=%u y=%p",
                AI_VICAP_CHN, frame.width, frame.height, frame.stride, frame.y);
        }

        ret = motion_adapter_process(&frame, &event, &has_event);
        if (!ret && has_event) {
            LOG("Motion detected: event_id=%u score=%u duration=%ums",
                event.event_id, event.motion_score, event.osd_duration_ms);
            osd_set_motion_visible(1, event.osd_duration_ms);
        }

        ret = ai_frame_release(handle);
        if (ret)
            LOG("ai_frame_release failed! ret=0x%x", ret);
    }

    LOG("AI motion thread stopped, frames=%u", frame_count);
    return NULL;
}

k_s32 ai_motion_thread_start(void)
{
    k_s32 ret;

    if (g_ai_motion_started)
        return 0;

    ret = ai_frame_channel_init();
    if (ret)
        return ret;

    ret = motion_adapter_init();
    if (ret) {
        ai_frame_channel_deinit();
        return ret;
    }

    g_ai_motion_running = 1;
    ret = pthread_create(&g_ai_motion_tid, NULL, ai_motion_thread, NULL);
    if (ret) {
        LOG("AI pthread_create failed! ret=%d", ret);
        g_ai_motion_running = 0;
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return ret;
    }

    g_ai_motion_started = K_TRUE;
    LOG("AI motion thread create OK");
    return 0;
}

void ai_motion_thread_stop(void)
{
    if (!g_ai_motion_started)
        return;

    g_ai_motion_running = 0;
    pthread_join(g_ai_motion_tid, NULL);
    g_ai_motion_started = K_FALSE;

    motion_adapter_deinit();
    ai_frame_channel_deinit();
    LOG("AI motion thread cleanup OK");
}
