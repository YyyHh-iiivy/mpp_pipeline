#include "mpp_pipeline.h"

#if VENC_OSD_ENABLE

#include "motion_detected_osd_512x96_argb8888.h"

/*
 * VENC 2D OSD 叠加模块。
 *
 * 这里不走 VO OSD，而是把 ARGB8888 图片放进 VB/MMZ buffer，
 * 再通过固定的 VENC 2D OSD region 叠加到编码通道 VENC_CHN 上。AI
 * 运动检测线程释放 AI frame 后只提交显隐请求；stream 线程通过
 * osd_poll_auto_hide() 串行更新素材 buffer，不在运行期修改 2D 参数。
 */
#define OSD_REGION_INDEX 0
#define OSD_START_X      32
#define OSD_START_Y      32
#define OSD_BUF_SIZE     ALIGN_UP(MOTION_DETECTED_OSD_SIZE, 0x1000)

#define OSD_HIDE_DEADLINE_GRACE_MS  20
#define OSD_APPLY_RETRY_MS          100

/* OSD 图片 buffer 和 VENC 2D 参数状态。 */
static k_bool g_osd_inited = K_FALSE;
static k_bool g_osd_2d_attached = K_FALSE;
static k_vb_blk_handle g_osd_block = VB_INVALID_HANDLE;
static k_u64 g_osd_phys_addr;
static void *g_osd_virt_addr;
static k_venc_2d_osd_attr g_osd_attr;

/* AI线程写目标状态，stream线程写已应用状态。 */
static k_u64 g_osd_hide_deadline_ms;
static k_u64 g_osd_retry_after_ms;
static k_u64 g_osd_request_generation;
static k_bool g_osd_desired_visible;
static k_bool g_osd_applied_visible;
static k_bool g_osd_success_logged[2];

/* 获取单调时间，避免系统时间调整影响 OSD 自动隐藏。 */
static k_u64 osd_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

/*
 * 只更新约 192KiB ARGB 素材；视频帧仍由 VENC 2D 硬件叠加。
 * 全局 alpha 固定为 255 时，全零 ARGB 像素的像素 alpha 为 0，表示隐藏。
 */
static k_s32 osd_update_buffer(k_bool visible)
{
    if (!g_osd_virt_addr || !g_osd_phys_addr)
        return -1;

    if (visible) {
        memcpy(g_osd_virt_addr,
               g_motion_detected_osd_argb8888,
               MOTION_DETECTED_OSD_SIZE);
    } else {
        memset(g_osd_virt_addr, 0, OSD_BUF_SIZE);
    }

    return kd_mpi_sys_mmz_flush_cache(g_osd_phys_addr,
                                      g_osd_virt_addr,
                                      OSD_BUF_SIZE);
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

    /* 2. cached mmap 后先清零素材，固定 region 初始保持透明。 */
    g_osd_virt_addr = kd_mpi_sys_mmap_cached(g_osd_phys_addr,
                                             OSD_BUF_SIZE);
    if (!g_osd_virt_addr) {
        LOG("kd_mpi_sys_mmap_cached(OSD) failed!");
        osd_release_buffer();
        return -1;
    }

    ret = osd_update_buffer(K_FALSE);
    if (ret) {
        LOG("osd_update_buffer(initial hidden) failed! ret=0x%x", ret);
        osd_release_buffer();
        return ret;
    }

    /* 3. region 参数固定；显隐仅由素材像素 alpha 决定。 */
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_attr.width        = MOTION_DETECTED_OSD_WIDTH;
    g_osd_attr.height       = MOTION_DETECTED_OSD_HEIGHT;
    g_osd_attr.startx       = OSD_START_X;
    g_osd_attr.starty       = OSD_START_Y;
    g_osd_attr.phys_addr[0] = (k_u32)g_osd_phys_addr;
    g_osd_attr.phys_addr[1] = 0;
    g_osd_attr.phys_addr[2] = 0;
    g_osd_attr.bg_alpha     = 0;
    g_osd_attr.osd_alpha    = 255;
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

    g_osd_hide_deadline_ms = 0;
    g_osd_retry_after_ms = 0;
    g_osd_request_generation = 0;
    g_osd_desired_visible = K_FALSE;
    g_osd_applied_visible = K_FALSE;
    memset(g_osd_success_logged, 0, sizeof(g_osd_success_logged));
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
    k_bool inited;
    k_u64 now_ms = osd_now_ms();
    k_bool target_visible = visible ? K_TRUE : K_FALSE;

    rt_enter_critical();
    inited = g_osd_inited;
    if (inited) {
        g_osd_desired_visible = target_visible;
        g_osd_hide_deadline_ms = (visible && duration_ms > 0) ?
                                 now_ms + duration_ms : 0;
        g_osd_retry_after_ms = 0;
        g_osd_request_generation++;
    }
    rt_exit_critical();

    if (!inited) {
        LOG("OSD motion visible=%u requested before osd_init", visible);
        return -1;
    }

    return 0;
}

k_s32 osd_poll_auto_hide(void)
{
    k_s32 ret;
    k_bool log_update;
    k_bool pending_after;
    k_bool target_visible;
    k_u64 apply_generation;
    k_u64 apply_start_ms;
    k_u64 apply_cost_ms;
    k_u64 now_ms = osd_now_ms();

    /* 临界区只抓取状态；约 192KiB 的素材更新必须在临界区外执行。 */
    rt_enter_critical();
    if (!g_osd_inited) {
        rt_exit_critical();
        return 0;
    }

    if (g_osd_hide_deadline_ms != 0 &&
        now_ms + OSD_HIDE_DEADLINE_GRACE_MS >= g_osd_hide_deadline_ms) {
        g_osd_hide_deadline_ms = 0;
        if (g_osd_desired_visible) {
            g_osd_desired_visible = K_FALSE;
            g_osd_retry_after_ms = 0;
            g_osd_request_generation++;
        }
    }

    if (g_osd_desired_visible == g_osd_applied_visible ||
        (g_osd_retry_after_ms != 0 && now_ms < g_osd_retry_after_ms)) {
        rt_exit_critical();
        return 0;
    }

    target_visible = g_osd_desired_visible;
    apply_generation = g_osd_request_generation;
    rt_exit_critical();

    apply_start_ms = osd_now_ms();
    ret = osd_update_buffer(target_visible);
    apply_cost_ms = osd_now_ms() - apply_start_ms;

    rt_enter_critical();
    if (!ret) {
        /* 硬件可见的是本次 flush 的素材；更新期间的新请求仍保持 pending。 */
        g_osd_applied_visible = target_visible;
        g_osd_retry_after_ms = 0;
    } else if (g_osd_request_generation == apply_generation) {
        g_osd_retry_after_ms = osd_now_ms() + OSD_APPLY_RETRY_MS;
    } else {
        /* buffer 更新期间产生了新请求，下一轮立即尝试最新目标。 */
        g_osd_retry_after_ms = 0;
    }
    pending_after = (g_osd_desired_visible != g_osd_applied_visible);
    rt_exit_critical();

    log_update = ret || !g_osd_success_logged[target_visible ? 1 : 0];
    if (!ret)
        g_osd_success_logged[target_visible ? 1 : 0] = K_TRUE;
    if (log_update) {
        LOG("[osd:buffer] generation=%llu visible=%u cost_ms=%llu ret=0x%x pending_after=%u",
            (unsigned long long)apply_generation,
            (unsigned int)target_visible,
            (unsigned long long)apply_cost_ms,
            ret,
            (unsigned int)pending_after);
    }

    return ret;
}

void osd_deinit(void)
{
    k_s32 ret;

    if (!g_osd_inited && !g_osd_2d_attached && g_osd_block == VB_INVALID_HANDLE)
        return;

    rt_enter_critical();
    g_osd_hide_deadline_ms = 0;
    g_osd_retry_after_ms = 0;
    g_osd_desired_visible = K_FALSE;
    g_osd_request_generation++;
    rt_exit_critical();

    if (g_osd_2d_attached) {
        LOG("VENC 2D OSD detach start");
        ret = kd_mpi_venc_detach_2d(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_2d_attached = K_FALSE;
        LOG("VENC 2D OSD detach done");
    }

    LOG("OSD buffer release start");
    osd_release_buffer();
    LOG("OSD buffer release done");
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_applied_visible = K_FALSE;
    memset(g_osd_success_logged, 0, sizeof(g_osd_success_logged));
    g_osd_inited = K_FALSE;
    LOG("VENC 2D OSD deinit OK");
}

#else /* VENC_OSD_ENABLE */

k_s32 osd_init(void)
{
    return 0;
}

k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms)
{
    (void)visible;
    (void)duration_ms;
    return 0;
}

k_s32 osd_poll_auto_hide(void)
{
    return 0;
}

void osd_deinit(void)
{
}

#endif /* VENC_OSD_ENABLE */
