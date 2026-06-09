#ifndef MPP_PIPELINE_H
#define MPP_PIPELINE_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "k_module.h"
#include "k_type.h"
#include "k_vb_comm.h"
#include "k_video_comm.h"
#include "k_sys_comm.h"
#include "mpi_vb_api.h"
#include "mpi_venc_api.h"
#include "mpi_sys_api.h"
#include "mpi_vicap_api.h"
#include "k_venc_comm.h"
#include "mpp_types.h"

/* GC2093 传感器 (开发板标注 TYS-2093-V31)
 * SDK 示例和多数 K230 板卡把 GC2093 接在 CSI2；CSI0 配置会在 kd_mpi_vicap_init()
 * 阶段出现 "iic read chip id err"。默认优先 CSI2，并在运行时回退尝试其它 CSI。
 */
#define SENSOR_TYPE  GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR  /* = 52 */

/* 编码参数 */
#define ENC_WIDTH    1920
#define ENC_HEIGHT   1080
#define ENC_BITRATE  4000        /* kbps */
#define SRC_FPS      30          /* VENC 通道输入帧率参数；当前 GC2093/VICAP 实际输入约 30fps */
#define DST_FPS      30          /* VENC 目标输出帧率参数；与 SRC_FPS 一致 */
// #define DST_FPS      15          /* VENC 目标输出帧率参数；不等同于 VICAP 硬件丢帧开关 */

/* VB 池配置 */
#define INPUT_BUF_CNT   6
#define OUTPUT_BUF_CNT  15

/* 对齐宏 */
#define ALIGN_UP(addr, size)  (((addr) + ((size) - 1U)) & (~((size) - 1U)))

/* 每块大小 (与 sample_venc.c 对齐) */
#define FRAME_BUF_SIZE   ALIGN_UP(ENC_WIDTH * ENC_HEIGHT * 2, 0xFFF)
#define STREAM_BUF_SIZE  ALIGN_UP(ENC_WIDTH * ENC_HEIGHT / 2, 0xFFF)
#define CHN_BUF_SIZE     ALIGN_UP(ENC_WIDTH * ENC_HEIGHT * 3 / 2, 0xFFF)

/* 自动退出时间 (秒) */
#define AUTO_EXIT_SEC   600

/* VENC 码流线程配置 */
#define VENC_MAX_PACKS                 MPP_MAX_STREAM_PACKS
#define VENC_GET_STREAM_TIMEOUT_MS     200

/* 通道 ID */
#define VENC_CHN    0
#define VICAP_DEV   VICAP_DEV_ID_0
#define VICAP_CHN   VICAP_CHN_ID_0

/* 管线状态机 — 用于清理时正确跳过未初始化的步骤 */
typedef enum {
    STATUS_IDLE = 0,
    STATUS_VB_INIT,
    STATUS_VENC_CREATED,
    STATUS_VENC_STARTED,
    STATUS_VICAP_INIT,
    STATUS_BINDED,
    STATUS_STREAM_STARTED,
    STATUS_RUNNING,
    STATUS_BUTT
} pipeline_status;

extern volatile int g_running;
extern pipeline_status g_status;
extern pthread_t g_stream_tid;

#define CHECK_RET(ret, func, line)  do { \
    if (ret) \
        printf("[ERROR] ret=0x%x, func=%s line=%d\n", ret, func, line); \
} while(0)

/* 带标签的日志 */
#define LOG(fmt, ...)  printf("[MPP] " fmt "\n", ##__VA_ARGS__)

k_s32 vb_init(void);
k_s32 venc_init(k_u32 chn, k_u32 bitrate);
k_s32 vicap_try_config(k_vicap_sensor_type sensor_type);
k_s32 vicap_config(k_vicap_sensor_type sensor_type);
k_s32 vi_bind_venc(void);
k_s32 vicap_start(void);
void *stream_thread(void *arg);
void pipeline_cleanup(void);

k_s32 stream_export_init(stream_export_mode mode);
k_s32 stream_export_submit(const mpp_stream_frame_desc *frame);
void stream_export_deinit(void);

k_s32 osd_init(void);
k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms);
void osd_deinit(void);

#endif /* MPP_PIPELINE_H */
