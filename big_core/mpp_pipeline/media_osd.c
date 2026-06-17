#include "mpp_pipeline.h"
#include "motion_detected_osd_512x96_argb8888.h"

/*
 * VENC 2D OSD 叠加模块。
 *
 * 这里不走 VO OSD，而是把 ARGB8888 图片放进 VB/MMZ buffer，
 * 再通过 VENC 2D OSD 参数叠加到编码通道 VENC_CHN 上。AI 运动检测
 * 线程只负责调用 osd_set_motion_visible()，本文件内部负责显隐、定时
 * 自动隐藏和退出释放。
 */
#define OSD_REGION_INDEX 0
#define OSD_START_X      32
#define OSD_START_Y      32
#define OSD_BUF_SIZE     ALIGN_UP(MOTION_DETECTED_OSD_SIZE, 0x1000)

#define OSD_THREAD_STACK_SIZE       4096
#define OSD_THREAD_PRIORITY         21
#define OSD_THREAD_TIMESLICE        10
#define OSD_THREAD_EXIT_POLL_MS     20
#define OSD_THREAD_EXIT_TIMEOUT_MS  1000
#define OSD_HIDE_DEADLINE_GRACE_MS  20

/* OSD 图片 buffer 和 VENC 2D 参数状态。 */
static k_bool g_osd_inited = K_FALSE;
static k_bool g_osd_2d_attached = K_FALSE;
static k_vb_blk_handle g_osd_block = VB_INVALID_HANDLE;
static k_u64 g_osd_phys_addr;
static void *g_osd_virt_addr;
static k_venc_2d_osd_attr g_osd_attr;

/* 自动隐藏控制资源。g_osd_lock 是二值信号量，当互斥锁使用。 */
static rt_sem_t g_osd_lock = RT_NULL;
static rt_timer_t g_osd_hide_timer = RT_NULL;
static rt_sem_t g_osd_hide_sem = RT_NULL;
static rt_sem_t g_osd_hide_exit_sem = RT_NULL;
static rt_thread_t g_osd_hide_tid = RT_NULL;
static volatile int g_osd_hide_running;

/* 非 0 表示当前有一次自动隐藏等待执行，同时用于过滤旧 timer 信号。 */
static k_u64 g_osd_hide_deadline_ms;

/* 获取单调时间，避免系统时间调整影响 OSD 自动隐藏。 */
static k_u64 osd_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

static k_s32 osd_set_alpha_locked(k_u32 alpha)
{
    k_s32 ret;

    /* 只修改全局透明度，图片 buffer 本身不变。调用者需持有 g_osd_lock。 */
    g_osd_attr.osd_alpha = alpha;
    ret = kd_mpi_venc_set_2d_osd_param(VENC_CHN, OSD_REGION_INDEX, &g_osd_attr);
    if (ret)
        LOG("kd_mpi_venc_set_2d_osd_param alpha=%u failed! ret=0x%x", alpha, ret);

    return ret;
}

static k_s32 osd_start_hide_timer_locked(k_u32 duration_ms)
{
    rt_tick_t timeout_ticks;
    k_s32 ret;

    /* 用一次性 timer 实现 duration_ms 到期后隐藏。调用者需持有 g_osd_lock。 */
    if (g_osd_hide_timer == RT_NULL || duration_ms == 0)
        return 0;

    timeout_ticks = rt_tick_from_millisecond(duration_ms);
    if (timeout_ticks == 0)
        timeout_ticks = 1;

    ret = rt_timer_control(g_osd_hide_timer, RT_TIMER_CTRL_SET_TIME, &timeout_ticks);
    if (ret != RT_EOK) {
        LOG("rt_timer_control(osdtmr) failed! ret=%d", ret);
        return ret;
    }

    ret = rt_timer_start(g_osd_hide_timer);
    if (ret != RT_EOK)
        LOG("rt_timer_start(osdtmr) failed! ret=%d", ret);

    return ret;
}

static void osd_hide_timer_cb(void *parameter)
{
    (void)parameter;

    /* timer 回调里不直接调用 MPP API，只唤醒线程去做隐藏动作。 */
    if (g_osd_hide_sem != RT_NULL)
        rt_sem_release(g_osd_hide_sem);
}

static void osd_hide_thread(void *arg)
{
    (void)arg;

    while (g_osd_hide_running) {
        k_s32 ret = rt_sem_take(g_osd_hide_sem, RT_WAITING_FOREVER);
        if (ret != RT_EOK)
            continue;

        if (!g_osd_hide_running)
            break;

        if (g_osd_lock != RT_NULL)
            rt_sem_take(g_osd_lock, RT_WAITING_FOREVER);

        if (g_osd_inited && g_osd_hide_deadline_ms != 0) {
            k_u64 now_ms = osd_now_ms();

            /*
             * 如果旧 timer 信号在续期后才被消费，不能立刻隐藏新一轮 OSD。
             * 到期或接近到期才隐藏；过早触发则按剩余时间重新挂一次 timer。
             */
            if (now_ms + OSD_HIDE_DEADLINE_GRACE_MS >= g_osd_hide_deadline_ms) {
                g_osd_hide_deadline_ms = 0;
                ret = osd_set_alpha_locked(0);
                if (!ret)
                    LOG("OSD auto hide alpha=0");
            } else {
                k_u64 remain_ms = g_osd_hide_deadline_ms - now_ms;
                if (remain_ms > 0xffffffffULL)
                    remain_ms = 0xffffffffULL;
                osd_start_hide_timer_locked((k_u32)remain_ms);
            }
        }

        if (g_osd_lock != RT_NULL)
            rt_sem_release(g_osd_lock);
    }

    if (g_osd_hide_exit_sem != RT_NULL)
        rt_sem_release(g_osd_hide_exit_sem);
}

static k_bool osd_wait_hide_thread_exit(k_u32 timeout_ms)
{
    k_u32 waited_ms = 0;

    /* 清理时等待隐藏线程自然退出，避免删除仍在使用的 RT 对象。 */
    if (g_osd_hide_exit_sem == RT_NULL)
        return K_TRUE;

    while (waited_ms < timeout_ms) {
        k_s32 ret = rt_sem_take(g_osd_hide_exit_sem,
                                rt_tick_from_millisecond(OSD_THREAD_EXIT_POLL_MS));
        if (ret == RT_EOK)
            return K_TRUE;

        waited_ms += OSD_THREAD_EXIT_POLL_MS;
    }

    LOG("OSD hide thread exit wait timeout after %ums", timeout_ms);
    return K_FALSE;
}

static k_s32 osd_control_init(void)
{
    k_s32 ret;

    /* 创建 OSD 控制资源：互斥信号量、隐藏通知、退出通知、一次性 timer 和工作线程。 */
    g_osd_lock = rt_sem_create("osdlock", 1, RT_IPC_FLAG_FIFO);
    if (g_osd_lock == RT_NULL) {
        LOG("rt_sem_create(osdlock) failed!");
        return -1;
    }

    g_osd_hide_sem = rt_sem_create("osdhsem", 0, RT_IPC_FLAG_FIFO);
    if (g_osd_hide_sem == RT_NULL) {
        LOG("rt_sem_create(osdhsem) failed!");
        return -1;
    }

    g_osd_hide_exit_sem = rt_sem_create("osdexit", 0, RT_IPC_FLAG_FIFO);
    if (g_osd_hide_exit_sem == RT_NULL) {
        LOG("rt_sem_create(osdexit) failed!");
        return -1;
    }

    g_osd_hide_timer = rt_timer_create("osdtmr",
                                       osd_hide_timer_cb,
                                       RT_NULL,
                                       1,
                                       RT_TIMER_FLAG_ONE_SHOT);
    if (g_osd_hide_timer == RT_NULL) {
        LOG("rt_timer_create(osdtmr) failed!");
        return -1;
    }

    g_osd_hide_running = 1;
    g_osd_hide_tid = rt_thread_create("osdhide",
                                      osd_hide_thread,
                                      RT_NULL,
                                      OSD_THREAD_STACK_SIZE,
                                      OSD_THREAD_PRIORITY,
                                      OSD_THREAD_TIMESLICE);
    if (g_osd_hide_tid == RT_NULL) {
        LOG("rt_thread_create(osdhide) failed!");
        g_osd_hide_running = 0;
        return -1;
    }

    ret = rt_thread_startup(g_osd_hide_tid);
    if (ret != RT_EOK) {
        LOG("rt_thread_startup(osdhide) failed! ret=%d", ret);
        g_osd_hide_running = 0;
        rt_thread_delete(g_osd_hide_tid);
        g_osd_hide_tid = RT_NULL;
        return ret;
    }

    return 0;
}

static void osd_control_deinit(void)
{
    /* 先停 timer，再唤醒线程退出，最后删除 RT 对象。 */
    if (g_osd_hide_timer != RT_NULL)
        rt_timer_stop(g_osd_hide_timer);

    if (g_osd_hide_tid != RT_NULL) {
        g_osd_hide_running = 0;
        if (g_osd_hide_sem != RT_NULL)
            rt_sem_release(g_osd_hide_sem);

        if (osd_wait_hide_thread_exit(OSD_THREAD_EXIT_TIMEOUT_MS))
            g_osd_hide_tid = RT_NULL;
    }

    if (g_osd_hide_timer != RT_NULL) {
        rt_timer_delete(g_osd_hide_timer);
        g_osd_hide_timer = RT_NULL;
    }

    if (g_osd_hide_exit_sem != RT_NULL && g_osd_hide_tid == RT_NULL) {
        rt_sem_delete(g_osd_hide_exit_sem);
        g_osd_hide_exit_sem = RT_NULL;
    }

    if (g_osd_hide_sem != RT_NULL && g_osd_hide_tid == RT_NULL) {
        rt_sem_delete(g_osd_hide_sem);
        g_osd_hide_sem = RT_NULL;
    }

    if (g_osd_lock != RT_NULL && g_osd_hide_tid == RT_NULL) {
        rt_sem_delete(g_osd_lock);
        g_osd_lock = RT_NULL;
    }

    g_osd_hide_deadline_ms = 0;
    g_osd_hide_running = 0;
}

static void osd_release_buffer(void)
{
    k_s32 ret;

    /* 释放顺序与申请顺序相反：先 munmap，再 release VB block。 */
    if (g_osd_virt_addr) {
        ret = kd_mpi_sys_munmap(g_osd_virt_addr, OSD_BUF_SIZE);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_virt_addr = NULL;
    }

    if (g_osd_block != VB_INVALID_HANDLE) {
        ret = kd_mpi_vb_release_block(g_osd_block);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_block = VB_INVALID_HANDLE;
    }

    g_osd_phys_addr = 0;
}

k_s32 osd_init(void)
{
    k_s32 ret;

    if (g_osd_inited)
        return 0;

    /* 1. 从公共 VB 池申请一块 4K 对齐的 OSD 图像 buffer。 */
    g_osd_block = kd_mpi_vb_get_block(VB_INVALID_POOLID,
                                      OSD_BUF_SIZE,
                                      NULL);
    if (g_osd_block == VB_INVALID_HANDLE) {
        LOG("kd_mpi_vb_get_block(OSD) failed! size=%u",
            (k_u32)OSD_BUF_SIZE);
        return -1;
    }

    g_osd_phys_addr = kd_mpi_vb_handle_to_phyaddr(g_osd_block);
    if (!g_osd_phys_addr) {
        LOG("kd_mpi_vb_handle_to_phyaddr(OSD) failed!");
        osd_release_buffer();
        return -1;
    }
    if (g_osd_phys_addr > 0xffffffffULL) {
        LOG("OSD phys addr out of 32-bit range: 0x%llx",
            (unsigned long long)g_osd_phys_addr);
        osd_release_buffer();
        return -1;
    }

    /* 2. cached mmap 后写入 ARGB8888 图片，再 flush cache 给硬件读取。 */
    g_osd_virt_addr = kd_mpi_sys_mmap_cached(g_osd_phys_addr,
                                             OSD_BUF_SIZE);
    if (!g_osd_virt_addr) {
        LOG("kd_mpi_sys_mmap_cached(OSD) failed!");
        osd_release_buffer();
        return -1;
    }

    memset(g_osd_virt_addr, 0, OSD_BUF_SIZE);
    memcpy(g_osd_virt_addr,
           g_motion_detected_osd_argb8888,
           MOTION_DETECTED_OSD_SIZE);

    ret = kd_mpi_sys_mmz_flush_cache(g_osd_phys_addr,
                                     g_osd_virt_addr,
                                     OSD_BUF_SIZE);
    if (ret) {
        LOG("kd_mpi_sys_mmz_flush_cache(OSD) failed! ret=0x%x", ret);
        osd_release_buffer();
        return ret;
    }

    /* 3. 组装 VENC 2D OSD 参数：初始 osd_alpha=0，等运动触发后再显示。 */
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_attr.width        = MOTION_DETECTED_OSD_WIDTH;
    g_osd_attr.height       = MOTION_DETECTED_OSD_HEIGHT;
    g_osd_attr.startx       = OSD_START_X;
    g_osd_attr.starty       = OSD_START_Y;
    g_osd_attr.phys_addr[0] = (k_u32)g_osd_phys_addr;
    g_osd_attr.phys_addr[1] = 0;
    g_osd_attr.phys_addr[2] = 0;
    g_osd_attr.bg_alpha     = 0;
    g_osd_attr.osd_alpha    = 0;
    g_osd_attr.video_alpha  = 255;
    g_osd_attr.add_order    = K_VENC_2D_ADD_ORDER_VIDEO_OSD;
    g_osd_attr.bg_color     = 0;
    g_osd_attr.fmt          = K_VENC_2D_OSD_FMT_ARGB8888;

    /* 4. 把 VENC 通道切到 2D OSD 模式，并下发 region 0 的 OSD 参数。 */
    ret = kd_mpi_venc_attach_2d(VENC_CHN);
    if (ret) {
        LOG("kd_mpi_venc_attach_2d failed! ret=0x%x", ret);
        osd_release_buffer();
        return ret;
    }
    g_osd_2d_attached = K_TRUE;

    ret = kd_mpi_venc_set_2d_mode(VENC_CHN, K_VENC_2D_CALC_MODE_OSD);
    if (ret) {
        LOG("kd_mpi_venc_set_2d_mode failed! ret=0x%x", ret);
        kd_mpi_venc_detach_2d(VENC_CHN);
        g_osd_2d_attached = K_FALSE;
        osd_release_buffer();
        return ret;
    }

    ret = kd_mpi_venc_set_2d_osd_param(VENC_CHN, OSD_REGION_INDEX, &g_osd_attr);
    if (ret) {
        LOG("kd_mpi_venc_set_2d_osd_param failed! ret=0x%x", ret);
        kd_mpi_venc_detach_2d(VENC_CHN);
        g_osd_2d_attached = K_FALSE;
        osd_release_buffer();
        return ret;
    }

    /* 5. OSD 参数下发成功后再启动自动隐藏控制线程。 */
    ret = osd_control_init();
    if (ret) {
        kd_mpi_venc_detach_2d(VENC_CHN);
        g_osd_2d_attached = K_FALSE;
        osd_control_deinit();
        osd_release_buffer();
        return ret;
    }

    g_osd_inited = K_TRUE;
    LOG("VENC 2D OSD init OK: %ux%u ARGB8888 at (%u,%u), phys=0x%llx",
        (k_u32)MOTION_DETECTED_OSD_WIDTH,
        (k_u32)MOTION_DETECTED_OSD_HEIGHT,
        (k_u32)OSD_START_X,
        (k_u32)OSD_START_Y,
        (unsigned long long)g_osd_phys_addr);
    return 0;
}

k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms)
{
    k_s32 ret;
    k_u32 target_alpha = visible ? 255 : 0;
    k_bool alpha_changed;

    if (g_osd_lock != RT_NULL)
        rt_sem_take(g_osd_lock, RT_WAITING_FOREVER);

    if (!g_osd_inited) {
        LOG("OSD motion visible=%u requested before osd_init", visible);
        if (g_osd_lock != RT_NULL)
            rt_sem_release(g_osd_lock);
        return -1;
    }

    if (g_osd_hide_timer != RT_NULL)
        rt_timer_stop(g_osd_hide_timer);

    /*
     * 消抖: AI 线程可能连续多帧检测到运动。
     * 如果 OSD 已经处于目标显隐状态，就不重复下发 VENC 2D 参数，
     * 只续期自动隐藏 timer，避免每帧 set_2d_osd_param() 堵住控制路径。
     */
    g_osd_hide_deadline_ms = 0;
    alpha_changed = (g_osd_attr.osd_alpha != target_alpha);
    if (alpha_changed) {
        ret = osd_set_alpha_locked(target_alpha);
        if (ret) {
            if (g_osd_lock != RT_NULL)
                rt_sem_release(g_osd_lock);
            return ret;
        }
    } else {
        ret = 0;
    }

    if (visible && duration_ms > 0) {
        g_osd_hide_deadline_ms = osd_now_ms() + duration_ms;
        ret = osd_start_hide_timer_locked(duration_ms);
        if (ret)
            g_osd_hide_deadline_ms = 0;
    }

    LOG("OSD motion visible=%u duration=%ums alpha=%u changed=%u",
        visible,
        duration_ms,
        (k_u32)g_osd_attr.osd_alpha,
        alpha_changed ? 1U : 0U);

    if (g_osd_lock != RT_NULL)
        rt_sem_release(g_osd_lock);

    return ret;
}

void osd_deinit(void)
{
    k_s32 ret;

    if (!g_osd_inited && !g_osd_2d_attached && g_osd_block == VB_INVALID_HANDLE)
        return;

    /* 清理时先停自动隐藏线程，确保后面 detach/release 时没有并发 set_param。 */
    osd_control_deinit();

    if (g_osd_2d_attached) {
        ret = kd_mpi_venc_detach_2d(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_2d_attached = K_FALSE;
    }

    osd_release_buffer();
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_inited = K_FALSE;
    LOG("VENC 2D OSD deinit OK");
}
