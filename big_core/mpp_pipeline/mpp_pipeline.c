/*
 * K230 MPP Pipeline MVP — Week 2 队长任务
 *
 * 单进程多线程验证硬件管线:
 *   Sensor(GC2093) -> VICAP(1080P,NV12) -> [sys_bind] -> VENC(H.265,CBR,15fps)
 *   采集线程: 打印 "Get NALU, Size: xxxx bytes"
 *
 * 验收标准: 15fps 持续打印 NALU 大小, 10 分钟不死机
 */

#include "mpp_pipeline.h"

volatile int g_running = 1;
pipeline_status g_status = STATUS_IDLE;
pthread_t g_stream_tid;

/* ================================================================
 * 信号处理 — 安全退出
 * ================================================================ */
static void sig_handler(int signo)
{
    LOG("Received signal %d, stopping...", signo);
    g_running = 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(void)
{
    k_s32 ret;

    printf("\n");
    printf("========================================\n");
    printf("  K230 MPP Pipeline MVP — Week 2\n");
    printf("  Sensor: GC2093(auto CSI)  Codec: H.265 CBR\n");
    printf("  Resolution: %dx%d@%dfps\n", ENC_WIDTH, ENC_HEIGHT, DST_FPS);
    printf("  Acceptance: 15fps NALU print, 10min stable\n");
    printf("========================================\n\n");

    /* 注册信号处理 */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Step 1: VB 池 */
    ret = vb_init();
    if (ret) goto cleanup;

    /* Step 2: VENC 编码通道 */
    ret = venc_init(VENC_CHN, ENC_BITRATE);
    if (ret) goto cleanup;

    ret = osd_init();
    if (ret) goto cleanup;

    /* Step 3: VICAP 摄像头配置 (GC2093) */
    ret = vicap_config(SENSOR_TYPE);
    if (ret) goto cleanup;

    /* Step 4: 硬件绑定 VI -> VENC */
    ret = vi_bind_venc();
    if (ret) goto cleanup;

    /* Step 5: 启动 VICAP 流 */
    ret = vicap_start();
    if (ret) goto cleanup;

    ret = stream_export_init(STREAM_EXPORT_LOCAL_LOG);
    if (ret) goto cleanup;

    g_status = STATUS_RUNNING;

    /* Step 5: 创建码流采集线程 */
    ret = pthread_create(&g_stream_tid, NULL, stream_thread, (void *)(k_u64)VENC_CHN);
    if (ret) {
        LOG("pthread_create failed! ret=%d", ret);
        goto cleanup;
    }

    LOG("Pipeline running. Press Ctrl+C to stop (auto-exit in %d seconds).", AUTO_EXIT_SEC);

    /* 主循环: 等待退出信号或超时 */
    {
        time_t start = time(NULL);
        while (g_running && difftime(time(NULL), start) < AUTO_EXIT_SEC) {
            sleep(1);
        }
        if (g_running) {
            LOG("Auto-exit after %d seconds.", AUTO_EXIT_SEC);
            g_running = 0;
        }
    }

    /* 等待采集线程退出 */
    pthread_join(g_stream_tid, NULL);

cleanup:
    pipeline_cleanup();

    if (ret == 0)
        LOG("Pipeline test PASSED.");
    else
        LOG("Pipeline test FAILED (ret=0x%x). Check error codes above.", ret);

    return ret;
}
