#include "mpp_pipeline.h"

static rt_thread_t g_ai_motion_tid = RT_NULL;
static rt_sem_t g_ai_motion_exit_sem = RT_NULL;
static volatile int g_ai_motion_running;
static k_bool g_ai_motion_started;

typedef struct {
    k_u64 thread_start_ms;
    k_u64 health_start_ms;
    k_u64 last_frame_ms;
    k_u32 dump_ok_total;
    k_u32 processed_total;
    k_u32 dump_fail_total;
    k_u32 release_fail_total;
    k_u32 health_dump_ok;
    k_u32 health_processed;
    k_u32 health_dump_fail;
    k_u32 health_release_fail;
} ai_motion_stats;

static k_u64 ai_motion_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

/*
 * Start the next acquisition interval from the end of the previous attempt.
 * This is intentionally separate from the pre-acquire gate: if dump, mmap,
 * or the motion algorithm takes longer than the interval, the next iteration
 * must wait a full interval instead of immediately catching up.
 */
static void ai_motion_schedule_next_acquire(k_u64 *next_acquire_ms)
{
    if (!next_acquire_ms)
        return;

    *next_acquire_ms = ai_motion_now_ms() +
                       (k_u64)AI_MOTION_ACQUIRE_INTERVAL_MS;
}

static k_bool ai_motion_wait_until_next_acquire(k_u64 *next_acquire_ms)
{
    if (!next_acquire_ms)
        return K_FALSE;

    while (g_ai_motion_running && g_running) {
        k_u64 now_ms = ai_motion_now_ms();

        if (*next_acquire_ms == 0 || now_ms >= *next_acquire_ms)
            return K_TRUE;

        {
            k_u64 remaining_ms = *next_acquire_ms - now_ms;
            k_u32 wait_ms = remaining_ms > AI_MOTION_WAIT_SLICE_MS ?
                            AI_MOTION_WAIT_SLICE_MS :
                            (k_u32)remaining_ms;

            rt_thread_mdelay(wait_ms);
        }
    }

    return K_FALSE;
}

static void ai_motion_stats_init(ai_motion_stats *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->thread_start_ms = ai_motion_now_ms();
    stats->health_start_ms = stats->thread_start_ms;
}

static void ai_motion_log_health(ai_motion_stats *stats)
{
    k_u64 now_ms = ai_motion_now_ms();
    k_u64 elapsed_ms = now_ms - stats->health_start_ms;

    if (elapsed_ms < AI_HEALTH_INTERVAL_MS)
        return;

    {
        double dump_fps = (double)stats->health_dump_ok * 1000.0 /
                          (double)elapsed_ms;
        double process_fps = (double)stats->health_processed * 1000.0 /
                             (double)elapsed_ms;
        k_u64 last_frame_age_ms = stats->last_frame_ms != 0 ?
                                  now_ms - stats->last_frame_ms : elapsed_ms;

        LOG("[health:ai] uptime_s=%llu dump_fps=%.1f process_fps=%.1f dump_ok=%u dump_fail=%u release_fail=%u last_frame_age_ms=%llu",
            (unsigned long long)((now_ms - stats->thread_start_ms) / 1000ULL),
            dump_fps,
            process_fps,
            stats->health_dump_ok,
            stats->health_dump_fail,
            stats->health_release_fail,
            (unsigned long long)last_frame_age_ms);
    }

    stats->health_start_ms = now_ms;
    stats->health_dump_ok = 0;
    stats->health_processed = 0;
    stats->health_dump_fail = 0;
    stats->health_release_fail = 0;
}

static k_bool wait_ai_motion_thread_exit(k_u32 timeout_ms)
{
    k_u32 waited_ms = 0;

    if (g_ai_motion_exit_sem == RT_NULL)
        return K_TRUE;

    while (waited_ms < timeout_ms) {
        k_s32 ret = rt_sem_take(g_ai_motion_exit_sem,
                                rt_tick_from_millisecond(THREAD_EXIT_POLL_MS));
        if (ret == RT_EOK)
            return K_TRUE;

        waited_ms += THREAD_EXIT_POLL_MS;
    }

    LOG("AI motion thread exit wait timeout after %ums", timeout_ms);
    return K_FALSE;
}

/*
 * AI motion worker thread.
 *
 * The loop owns the full per-frame lifecycle:
 * 1. A monotonic 200ms gate runs before any AI frame is acquired.
 * 2. ai_frame_try_get() dumps and maps one AI-channel Y plane.
 * 3. Every acquired frame is processed and immediately released.
 * 4. OSD and snapshot requests are issued only after the frame is released.
 */
static void ai_motion_thread(void *arg)
{
    k_u32 timeout_count = 0;
    k_u64 next_acquire_ms = 0;
    ai_motion_stats stats;

    (void)arg;
    ai_motion_stats_init(&stats);
    LOG("AI motion thread started");
    LOG("AI motion acquire config: dump_fps=%u process_fps=%u interval_ms=%u wait_slice_ms=%u",
        (k_u32)AI_MOTION_ACQUIRE_FPS,
        (k_u32)AI_MOTION_ACQUIRE_FPS,
        (k_u32)AI_MOTION_ACQUIRE_INTERVAL_MS,
        (k_u32)AI_MOTION_WAIT_SLICE_MS);

    while (g_ai_motion_running && g_running) {
        k_s32 ret;
        ai_gray_frame_view frame;
        motion_event_msg event;
        k_bool has_event = K_FALSE;
        void *ai_frame_handle = NULL;
        k_s32 release_ret;

        if (!ai_motion_wait_until_next_acquire(&next_acquire_ms))
            break;

        ret = ai_frame_try_get(&frame, &ai_frame_handle);
        if (ret) {
            stats.dump_fail_total++;
            stats.health_dump_fail++;
            if ((timeout_count++ % 100) == 0)
                LOG("AI frame dump timeout/no frame ret=0x%x", ret);
            ai_motion_schedule_next_acquire(&next_acquire_ms);
            ai_motion_log_health(&stats);
            continue;
        }
        timeout_count = 0;
        stats.dump_ok_total++;
        stats.health_dump_ok++;

        stats.processed_total++;
        stats.health_processed++;
        ret = motion_adapter_process(&frame, &event, &has_event);

        release_ret = ai_frame_release(ai_frame_handle);
        stats.last_frame_ms = ai_motion_now_ms();
        if (release_ret) {
            stats.release_fail_total++;
            stats.health_release_fail++;
            LOG("ai_frame_release failed! ret=0x%x", release_ret);
        }

        /* 以释放完成时刻重新计时，禁止处理超时后的突发取帧。 */
        ai_motion_schedule_next_acquire(&next_acquire_ms);

        ai_motion_log_health(&stats);

        if (!ret && !release_ret && has_event && g_ai_motion_running && g_running) {
            k_s32 osd_ret;
            k_s32 snapshot_ret = 0;
            snapshot_request_msg snapshot_req;

            osd_ret = osd_set_motion_visible(1, event.osd_duration_ms);

            if (event.request_snapshot) {
                memset(&snapshot_req, 0, sizeof(snapshot_req));
                snapshot_req.event_id = event.event_id;
                snapshot_req.frame_time_ms = event.detect_time_ms;
                snapshot_req.source_chn = VENC_CHN;
                snprintf(snapshot_req.path_hint,
                         sizeof(snapshot_req.path_hint),
                         "motion-event-%u",
                         event.event_id);

                snapshot_ret = stream_export_request_snapshot(&snapshot_req);
            }

            LOG("[event:motion] id=%u score=%u duration_ms=%u osd_enabled=%u osd_ret=0x%x snapshot_ret=0x%x",
                event.event_id,
                event.motion_score,
                event.osd_duration_ms,
                (k_u32)VENC_OSD_ENABLE,
                osd_ret,
                snapshot_ret);
        }
    }

    {
        k_u64 elapsed_ms = ai_motion_now_ms() - stats.thread_start_ms;
        double dump_fps = elapsed_ms > 0 ?
            (double)stats.dump_ok_total * 1000.0 / (double)elapsed_ms : 0.0;
        double process_fps = elapsed_ms > 0 ?
            (double)stats.processed_total * 1000.0 / (double)elapsed_ms : 0.0;

        LOG("AI motion thread stopped, frames=%u dump_frames=%u processed_frames=%u skipped_frames=0 dump_fail=%u release_fail=%u actual_dump_fps=%.1f actual_process_fps=%.1f",
            stats.dump_ok_total,
            stats.dump_ok_total,
            stats.processed_total,
            stats.dump_fail_total,
            stats.release_fail_total,
            dump_fps,
            process_fps);
    }
    if (g_ai_motion_exit_sem != RT_NULL)
        rt_sem_release(g_ai_motion_exit_sem);
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

    g_ai_motion_exit_sem = rt_sem_create("aidone", 0, RT_IPC_FLAG_FIFO);
    if (g_ai_motion_exit_sem == RT_NULL) {
        LOG("rt_sem_create(aidone) failed!");
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return -1;
    }

    g_ai_motion_running = 1;
    g_ai_motion_tid = rt_thread_create("ai_motion",
                                       ai_motion_thread,
                                       RT_NULL,
                                       AI_THREAD_STACK_SIZE,
                                       AI_THREAD_PRIORITY,
                                       AI_THREAD_TIMESLICE);
    if (g_ai_motion_tid == RT_NULL) {
        LOG("rt_thread_create(ai_motion) failed!");
        g_ai_motion_running = 0;
        rt_sem_delete(g_ai_motion_exit_sem);
        g_ai_motion_exit_sem = RT_NULL;
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return -1;
    }

    ret = rt_thread_startup(g_ai_motion_tid);
    if (ret != RT_EOK) {
        LOG("rt_thread_startup(ai_motion) failed! ret=%d", ret);
        g_ai_motion_running = 0;
        rt_thread_delete(g_ai_motion_tid);
        g_ai_motion_tid = RT_NULL;
        rt_sem_delete(g_ai_motion_exit_sem);
        g_ai_motion_exit_sem = RT_NULL;
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
    if (g_ai_motion_exit_sem != RT_NULL) {
        if (wait_ai_motion_thread_exit(AI_THREAD_EXIT_TIMEOUT_MS)) {
            rt_sem_delete(g_ai_motion_exit_sem);
            g_ai_motion_exit_sem = RT_NULL;
        } else {
            LOG("AI motion thread still stopping; continue cleanup");
        }
    }
    if (g_ai_motion_exit_sem == RT_NULL) {
        g_ai_motion_tid = RT_NULL;
        g_ai_motion_started = K_FALSE;

        motion_adapter_deinit();
        ai_frame_channel_deinit();
        LOG("AI motion thread cleanup OK");
    }
}
