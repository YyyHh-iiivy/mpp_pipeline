#include "mpp_pipeline.h"

/* ================================================================
 * Step 1: VB 内存池初始化
 *
 * 内存计算推导:
 *   1080P NV12 单帧含 padding:
 *     FRAME_BUF_SIZE = (1920*1080*2 + 0xFFF) & ~0xFFF = 0x3F5000 ≈ 4.0 MB
 *   编码输出流 buffer:
 *     STREAM_BUF_SIZE = (1920*1080/2 + 0xFFF) & ~0xFFF = 0xFD800 ≈ 1.0 MB
 *   每个 channel 的帧 buffer:
 *     CHN_BUF_SIZE = (1920*1080*3/2 + 0xFFF) & ~0xFFF = 0x2F8000 ≈ 3.0 MB
 *   AI 低清旁路 buffer:
 *     AI_CHN_BUF_SIZE = ALIGN_UP(640*480*3/2, 0x1000) = 0x71000 ≈ 450 KB
 *   OSD ARGB8888 buffer:
 *     OSD_BUF_SIZE = ALIGN_UP(512*96*4, 0x1000) = 0x30000 = 192 KB
 *
 *   主链路为 3*4MB + 3*1MB ≈ 15 MB；AI 启用后增加约 2.7 MB。
 *   OSD 启用时再增加一个 192 KB 的独立 pool，不能消耗 VENC 的三块 stream buffer。
 * ================================================================ */
k_s32 vb_init(void)
{
    k_s32 ret;
    k_vb_config config;

    memset(&config, 0, sizeof(config));
#if AI_BRANCH_ENABLE && VENC_OSD_ENABLE
    config.max_pool_cnt = 4;
#elif AI_BRANCH_ENABLE || VENC_OSD_ENABLE
    config.max_pool_cnt = 3;
#else
    config.max_pool_cnt = 2;
#endif

    /* Pool 0: 输入帧 buffer */
    config.comm_pool[0].blk_cnt  = INPUT_BUF_CNT;
    config.comm_pool[0].blk_size = FRAME_BUF_SIZE;
    config.comm_pool[0].mode     = VB_REMAP_MODE_NOCACHE;

    /* Pool 1: 编码输出码流 buffer */
    config.comm_pool[1].blk_cnt  = OUTPUT_BUF_CNT;
    config.comm_pool[1].blk_size = STREAM_BUF_SIZE;
    config.comm_pool[1].mode     = VB_REMAP_MODE_NOCACHE;

#if AI_BRANCH_ENABLE
    /* Pool 2: AI 低清旁路帧 buffer */
    config.comm_pool[2].blk_cnt  = AI_BUF_CNT;
    config.comm_pool[2].blk_size = AI_CHN_BUF_SIZE;
    config.comm_pool[2].mode     = VB_REMAP_MODE_NOCACHE;
#endif

#if VENC_OSD_ENABLE
    /* 独立 OSD pool：AI-off 时为 pool 2，AI-on 时为 pool 3。 */
    config.comm_pool[OSD_POOL_INDEX].blk_cnt  = OSD_BUF_CNT;
    config.comm_pool[OSD_POOL_INDEX].blk_size = OSD_BUF_SIZE;
    config.comm_pool[OSD_POOL_INDEX].mode     = VB_REMAP_MODE_NOCACHE;
#endif

    ret = kd_mpi_vb_set_config(&config);
    if (ret) {
        LOG("kd_mpi_vb_set_config failed! ret=0x%x", ret);
        return ret;
    }

    ret = kd_mpi_vb_init();
    if (ret) {
        LOG("kd_mpi_vb_init failed! ret=0x%x", ret);
        return ret;
    }

    LOG("VB pool init OK");
    g_status = STATUS_VB_INIT;
    return 0;
}
