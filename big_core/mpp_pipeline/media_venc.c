#include "mpp_pipeline.h"

static k_u64 venc_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

static k_u64 venc_stream_video_pts(const k_venc_stream *stream)
{
    if (!stream || !stream->pack)
        return 0;

    for (k_u32 i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].type != K_VENC_HEADER && stream->pack[i].pts != 0)
            return stream->pack[i].pts;
    }

    return 0;
}

static const char *venc_stall_state(compact_diag_action action)
{
    switch (action) {
    case COMPACT_DIAG_STALL_START:
        return "start";
    case COMPACT_DIAG_STALL_ONGOING:
        return "ongoing";
    case COMPACT_DIAG_STALL_RECOVERED:
        return "recovered";
    default:
        return "normal";
    }
}

static void venc_log_stall(compact_diag_action action,
                           const char *cause,
                           k_s32 stall_ret,
                           k_u64 elapsed_ms,
                           k_u64 last_seq,
                           k_u64 last_pts_age_ms,
                           k_u64 last_pts,
                           k_u64 current_pts,
                           k_u32 cur_packs,
                           k_u32 pic_cnt,
                           k_u32 pic_bytes,
                           k_u32 mean_qp,
                           k_u32 pending)
{
    k_u64 pts_gap_us = 0;

    if (action == COMPACT_DIAG_NONE)
        return;
    if (current_pts > last_pts && last_pts != 0)
        pts_gap_us = current_pts - last_pts;

    LOG("[stall:venc] state=%s cause=%s ret=0x%x elapsed_ms=%llu last_seq=%llu last_pts_age_ms=%llu last_pts=%llu current_pts=%llu pts_gap_us=%llu cur_packs=%u pic_cnt=%u pic_bytes=%u mean_qp=%u pending=%u",
        venc_stall_state(action), cause ? cause : "unknown", stall_ret,
        (unsigned long long)elapsed_ms,
        (unsigned long long)last_seq,
        (unsigned long long)last_pts_age_ms,
        (unsigned long long)last_pts,
        (unsigned long long)current_pts,
        (unsigned long long)pts_gap_us,
        cur_packs, pic_cnt, pic_bytes, mean_qp, pending);
}

k_s32 venc_request_idr_once(k_u32 chn, const char *reason)
{
#if VENC_FORCE_IDR_ENABLE
    k_s32 ret;

#if VENC_FORCE_IDR_USE_MAPI
    ret = kd_mapi_venc_request_idr((k_s32)chn);
#else
    ret = kd_mpi_venc_request_idr(chn);
#endif
    LOG("VENC request IDR chn=%u reason=%s ret=0x%x",
        chn,
        reason ? reason : "manual",
        ret);
    return ret;
#else
    static k_u32 log_count;

    log_count++;
    if (log_count == 1U || (log_count % 30U) == 0U) {
        LOG("VENC request IDR skipped chn=%u reason=%s: VENC_FORCE_IDR_ENABLE=0, GOP fallback=%u",
            chn,
            reason ? reason : "manual",
            VENC_GOP);
    }
    return -1;
#endif
}

/* ================================================================
 * Step 2: VENC 编码通道配置
 *
 * H.265 Main Profile, CBR 1500kbps
 *
 * 注意: src_frame_rate/dst_frame_rate 是 VENC 码率控制/目标帧率参数；
 * 当前 VICAP->VENC 硬件绑定链路是否实际降帧，要以 stream_thread 统计为准。
 * ================================================================ */
/**
 * @brief 创建VENC（视频编码）通道
 *
 * 该函数只负责创建H.265编码通道，配置为CBR码率控制模式。
 * src_frame_rate/dst_frame_rate 按官方定义分别表示VENC通道输入帧率和目标帧率，
 * 但它们不是VICAP侧的硬件丢帧配置。
 *
 * @param chn 通道号
 * @param bitrate 目标码率（kbps）
 * @return k_s32 成功返回0，失败返回错误码
 */
k_s32 venc_create_chn(k_u32 chn, k_u32 bitrate)
{
    k_s32 ret;                                  // 返回值，用于存储API调用结果
    k_venc_chn_attr attr;                       // 视频编码通道属性结构体

    memset(&attr, 0, sizeof(attr));             // 清零编码通道属性结构体，确保所有字段初始化为0

    /* 编码器属性 */
    attr.venc_attr.type            = K_PT_H265;   // 设置编码类型为H.265
    attr.venc_attr.profile         = VENC_PROFILE_H265_MAIN;  // 设置编码配置为Main Profile
    attr.venc_attr.pic_width       = ENC_WIDTH;   // 设置编码图片宽度
    attr.venc_attr.pic_height      = ENC_HEIGHT;  // 设置编码图片高度
    attr.venc_attr.stream_buf_size = STREAM_BUF_SIZE;  // 设置码流缓冲区大小
    attr.venc_attr.stream_buf_cnt  = OUTPUT_BUF_CNT;   // 设置码流缓冲区数量

    /* 码率控制: CBR */
    attr.rc_attr.rc_mode           = K_VENC_RC_MODE_CBR;  // 设置码率控制模式为CBR（恒定比特率）
    attr.rc_attr.cbr.src_frame_rate = SRC_FPS;    // VENC通道输入帧率参数，与VICAP主通道请求节拍一致
    attr.rc_attr.cbr.dst_frame_rate = DST_FPS;    // VENC目标输出帧率参数；不负责配置VICAP硬件丢帧
    attr.rc_attr.cbr.bit_rate       = bitrate;    // 设置目标码率为指定值
    attr.rc_attr.cbr.gop            = VENC_GOP;  /* 默认8帧，15fps时自然I帧间隔约533ms */

    ret = kd_mpi_venc_create_chn(chn, &attr);     // 创建VENC通道，使用指定的通道号和属性
    if (ret) {                                  // 检查通道创建是否成功
        LOG("kd_mpi_venc_create_chn failed! ret=0x%x", ret);  // 记录创建失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_VENC_CREATED;             // 更新全局状态为VENC已创建

    LOG("VENC chn=%u create OK", chn);         // 记录创建成功的日志
    LOG("Low-latency VENC config: src_fps=%u dst_fps=%u vicap_fps=%u bitrate_kbps=%u gop=%u input_buf=%u output_buf=%u drift_drop_ms=%lld drift_clear_ms=%lld",
        SRC_FPS,
        DST_FPS,
        VICAP_OUTPUT_FPS,
        bitrate,
        VENC_GOP,
        INPUT_BUF_CNT,
        OUTPUT_BUF_CNT,
        (long long)STREAM_FRESHNESS_DROP_THRESHOLD_MS,
        (long long)STREAM_FRESHNESS_CLEAR_THRESHOLD_MS);
    return 0;                                   // 返回成功状态
}

/* ================================================================
 * 启动 VENC 编码通道
 * ================================================================ */
k_s32 venc_start_chn(k_u32 chn)
{
    k_s32 ret;

    ret = kd_mpi_venc_start_chn(chn);           // 启动VENC通道
    if (ret) {                                  // 检查通道启动是否成功
        LOG("kd_mpi_venc_start_chn failed! ret=0x%x", ret);  // 记录启动失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_VENC_STARTED;             // 更新全局状态为VENC已启动

    LOG("VENC chn=%u start OK", chn);          // 记录启动成功的日志
    return 0;                                   // 返回成功状态
}

/* ================================================================
 * Step 5: 码流采集线程
 *
 * 从 VENC 获取编码后的 NALU, 打印大小
 * 实际输出帧率以这里的统计为准；当前硬件绑定链路实测约 30fps。
 * ================================================================ */
/**
 * @brief 码流处理线程函数
 * 
 * 该函数在一个独立线程中运行，持续从VENC模块获取编码后的NALU（网络抽象层单元），
 * 并统计帧数、字节数等信息，定期输出性能统计信息
 * 
 * @param arg 传入的参数，转换为通道号
 * @return void* 线程返回值（始终为NULL）
 */
void stream_thread(void *arg)
{
    k_u32 chn = (k_u32)(k_u64)arg;              // 将传入参数转换为通道号
    k_venc_stream output;                        // 存储编码后的码流数据
    k_venc_pack packs[VENC_MAX_PACKS];           // SDK get_stream() 的临时 pack 描述缓冲，不承载码流数据
    k_s32 ret;                                  // 返回值，用于存储API调用结果
    k_u32 frame_count = 0;                      // 总帧计数器，统计接收到的所有非头部帧
    k_u64 total_bytes = 0;                      // 总字节数计数器
    k_u32 flush_fail_count = 0;                 // 独立release drain失败计数
    k_u32 submit_fail_count = 0;
    k_s64 source_drift_ms = 0;
    stream_freshness_tracker freshness;
    compact_diag_tracker compact_diag;
    k_u64 stream_start_ms = venc_now_ms();
    k_u64 last_valid_stream_ms = stream_start_ms;
    k_u64 health_interval_start_ms = stream_start_ms;
    k_u64 last_valid_pts = 0;
    k_u32 health_interval_frames = 0;
    k_u64 health_interval_bytes = 0;
    k_u32 health_interval_max_frame_bytes = 0;
    k_u64 health_interval_pts_delta_min_us = 0;
    k_u64 health_interval_pts_delta_max_us = 0;
    k_u64 pack_overflow_count = 0;
    k_u32 max_query_packs = 0;
    k_u32 pack_alloc_fail_count = 0;
    k_u64 stall_elapsed = 0;

    LOG("Stream thread started");               // 记录线程启动日志
    LOG("[diag] compact_diag normal=60s stall=500ms anomaly=1s");
    memset(&output, 0, sizeof(output));
    output.pack = packs;
    stream_freshness_init(&freshness);
    compact_diag_tracker_init(&compact_diag, stream_start_ms);

    while (g_stream_running) {                  // cleanup 明确停止前持续 drain VENC
        k_venc_chn_status status;               // VENC通道状态结构体
        k_bool release_by_caller = K_TRUE;       // 导出层返回的释放责任
        compact_diag_action diag_action;
        k_u64 now_ms;
        k_u64 video_pts = 0;
        k_u32 query_pack_cnt;
        k_venc_pack *dynamic_packs = NULL;

        memset(&status, 0, sizeof(status));

        /* 在VENC query/get前串行完成固定OSD region的素材buffer更新。 */
        (void)osd_poll_auto_hide();

        /*
         * READ_DONE回调必须独立于下一帧有效码流推进。否则VENC暂时返回
         * 零pack时，最后一个pending流无法释放，会形成永久无流的闭环。
         */
        ret = stream_export_flush();
        if (ret) {
            if ((flush_fail_count++ % 25) == 0)
                LOG("stream_export_flush failed! ret=0x%x", ret);
        } else {
            flush_fail_count = 0;
        }

        /* 查询当前有多少个 pack */
        ret = kd_mpi_venc_query_status(chn, &status);  // 查询VENC通道的状态
        if (ret) {                              // 检查查询是否成功
            now_ms = venc_now_ms();
            diag_action = compact_diag_observe(&compact_diag, now_ms, 0, &stall_elapsed);
            venc_log_stall(diag_action, "query_fail", ret, stall_elapsed,
                           stream_export_get_last_seq(),
                           now_ms - last_valid_stream_ms,
                           last_valid_pts, 0,
                           0, 0, 0, 0,
                           stream_export_get_pending_count());
            if (!g_stream_running)
                break;
            rt_thread_mdelay(50);               // 休眠50毫秒后重试
            continue;                           // 继续下一轮循环
        }

        memset(&output, 0, sizeof(output));
        query_pack_cnt = status.cur_packs ? status.cur_packs : 1U;
        if (query_pack_cnt > max_query_packs)
            max_query_packs = query_pack_cnt;

        if (query_pack_cnt > VENC_MAX_PACKS) {
            dynamic_packs = (k_venc_pack *)calloc(query_pack_cnt, sizeof(*dynamic_packs));
            if (!dynamic_packs) {
                pack_alloc_fail_count++;
                if (pack_alloc_fail_count == 1U ||
                    (pack_alloc_fail_count % 30U) == 0U) {
                    LOG("VENC pack descriptor alloc failed query_packs=%u count=%u",
                        query_pack_cnt,
                        pack_alloc_fail_count);
                }
                rt_thread_mdelay(5);
                continue;
            }
            output.pack = dynamic_packs;
        } else {
            memset(packs, 0, sizeof(packs));
            output.pack = packs;
        }
        output.pack_cnt = query_pack_cnt;

        /* 有限等待编码完成，保证信号退出和 cleanup 能回收线程 */
        ret = kd_mpi_venc_get_stream(chn, &output, VENC_GET_STREAM_TIMEOUT_MS);
        if (ret) {                              // 检查获取码流是否成功
            now_ms = venc_now_ms();
            diag_action = compact_diag_observe(&compact_diag, now_ms, 0, &stall_elapsed);
            venc_log_stall(diag_action, "get_stream_fail", ret, stall_elapsed,
                           stream_export_get_last_seq(),
                           now_ms - last_valid_stream_ms,
                           last_valid_pts, 0,
                           status.cur_packs,
                           status.stream_info.u32PicCnt,
                           status.stream_info.u32PicBytesNum,
                           status.stream_info.u32MeanQp,
                           stream_export_get_pending_count());
            free(dynamic_packs);
            if (!g_stream_running)               // 如果 cleanup 要求退出
                break;                           // 跳出循环结束线程
            continue;                            // 暂时无流时继续等待
        }

        if (output.pack_cnt == 0) {
            now_ms = venc_now_ms();
            diag_action = compact_diag_observe(&compact_diag, now_ms, 0, &stall_elapsed);
            venc_log_stall(diag_action, "zero_pack", ret, stall_elapsed,
                           stream_export_get_last_seq(),
                           now_ms - last_valid_stream_ms,
                           last_valid_pts, 0,
                           status.cur_packs,
                           status.stream_info.u32PicCnt,
                           status.stream_info.u32PicBytesNum,
                           status.stream_info.u32MeanQp,
                           stream_export_get_pending_count());
            free(dynamic_packs);
            rt_thread_mdelay(5);
            continue;
        }

        now_ms = venc_now_ms();
        video_pts = venc_stream_video_pts(&output);
        diag_action = compact_diag_observe(&compact_diag, now_ms, 1, &stall_elapsed);
        venc_log_stall(diag_action, "valid_stream", ret, stall_elapsed,
                       stream_export_get_last_seq(),
                       now_ms - last_valid_stream_ms,
                       last_valid_pts, video_pts,
                       status.cur_packs,
                       status.stream_info.u32PicCnt,
                       status.stream_info.u32PicBytesNum,
                       status.stream_info.u32MeanQp,
                       stream_export_get_pending_count());
        last_valid_stream_ms = now_ms;
        if (video_pts != 0) {
            if (last_valid_pts != 0 && video_pts > last_valid_pts) {
                k_u64 pts_delta_us = video_pts - last_valid_pts;

                if (health_interval_pts_delta_min_us == 0 ||
                    pts_delta_us < health_interval_pts_delta_min_us) {
                    health_interval_pts_delta_min_us = pts_delta_us;
                }
                if (pts_delta_us > health_interval_pts_delta_max_us)
                    health_interval_pts_delta_max_us = pts_delta_us;
            }
            last_valid_pts = video_pts;
        }

        if (!g_stream_running) {
            ret = kd_mpi_venc_release_stream(chn, &output);
            if (ret)
                LOG("kd_mpi_venc_release_stream failed! ret=0x%x", ret);
            free(dynamic_packs);
            break;
        }

        if (query_pack_cnt > VENC_MAX_PACKS ||
            output.pack_cnt > VENC_MAX_PACKS) {
            k_u64 overflow_total_bytes = 0;

            for (k_u32 i = 0; i < output.pack_cnt; i++)
                overflow_total_bytes += output.pack[i].len;

            pack_overflow_count++;
            if (pack_overflow_count == 1ULL ||
                (pack_overflow_count % 30ULL) == 0ULL) {
                LOG("[venc:pack-overflow] query_packs=%u returned_packs=%u total_bytes=%llu pts=%llu count=%llu max_query_packs=%u",
                    query_pack_cnt,
                    output.pack_cnt,
                    (unsigned long long)overflow_total_bytes,
                    (unsigned long long)video_pts,
                    (unsigned long long)pack_overflow_count,
                    max_query_packs);
            }

            ret = kd_mpi_venc_release_stream(chn, &output);
            if (ret) {
                LOG("kd_mpi_venc_release_stream pack_overflow failed! ret=0x%x",
                    ret);
            }
            free(dynamic_packs);
            rt_thread_mdelay(1);
            continue;
        }

        {
            k_bool stale_drop = K_FALSE;

            ret = stream_freshness_observe(&freshness,
                                           video_pts,
                                           venc_now_ms(),
                                           &source_drift_ms,
                                           &stale_drop);
            if (ret) {
                LOG("stream_freshness_observe failed! ret=0x%x", ret);
            } else if (stale_drop) {
                ret = kd_mpi_venc_release_stream(chn, &output);
                if (ret) {
                    LOG("kd_mpi_venc_release_stream stale_drop failed! ret=0x%x", ret);
                }
                free(dynamic_packs);

                if (freshness.stale_drop_count == 1 ||
                    (freshness.stale_drop_count % 30ULL) == 0) {
                    LOG("VENC stale_drop: count=%llu source_drift_ms=%lld max_drift_ms=%lld catch_up=%u",
                        (unsigned long long)freshness.stale_drop_count,
                        (long long)source_drift_ms,
                        (long long)freshness.max_drift_ms,
                        (unsigned int)freshness.catching_up);
                }
                continue;
            }
        }

        /* 遍历所有 pack, 只统计非 header 的数据帧 */
        for (k_u32 i = 0; i < output.pack_cnt; i++) {  // 遍历所有编码包
            if (output.pack[i].type != K_VENC_HEADER) {  // 检查是否为非头部帧（即实际数据帧）
                frame_count++;                  // 增加总帧计数
                total_bytes += output.pack[i].len;  // 累加数据长度
                health_interval_frames++;
                health_interval_bytes += output.pack[i].len;
                if (output.pack[i].len > health_interval_max_frame_bytes)
                    health_interval_max_frame_bytes = output.pack[i].len;
            }
        }

        ret = stream_export_submit_venc_stream(chn, &output, &release_by_caller);
        if (ret) {
            submit_fail_count++;
            if (submit_fail_count == 1 || (submit_fail_count % 15U) == 0) {
                LOG("stream_export_submit_venc_stream failed! ret=0x%x status_cur_packs=%u output_pack_cnt=%u release_by_caller=%u pending=%u failures=%u",
                    ret,
                    status.cur_packs,
                    output.pack_cnt,
                    (unsigned int)release_by_caller,
                    stream_export_get_pending_count(),
                    submit_fail_count);
            }
        }

        if (release_by_caller) {
            ret = kd_mpi_venc_release_stream(chn, &output);  // 释放已获取的码流资源
            if (ret) {                              // 检查释放操作是否成功
                LOG("kd_mpi_venc_release_stream failed! ret=0x%x", ret);  // 记录错误日志
            }
        }
        free(dynamic_packs);

        if (!g_stream_running)
            break;

        now_ms = venc_now_ms();
        if (now_ms - health_interval_start_ms >=
            VENC_HEALTH_INTERVAL_MS) {
            k_u64 health_elapsed_ms = now_ms - health_interval_start_ms;
            double health_fps = health_elapsed_ms > 0 ?
                (double)health_interval_frames * 1000.0 /
                (double)health_elapsed_ms : 0.0;
            double health_kbps = health_elapsed_ms > 0 ?
                (double)health_interval_bytes * 8.0 /
                (double)health_elapsed_ms : 0.0;
            double health_avg_frame_bytes = health_interval_frames > 0 ?
                (double)health_interval_bytes /
                (double)health_interval_frames : 0.0;
            LOG("[health:venc] uptime_s=%llu fps=%.1f kbps=%.1f interval_bytes=%llu avg_frame_bytes=%.1f max_frame_bytes=%u pts_delta_min_us=%llu pts_delta_max_us=%llu mean_qp=%u total_frames=%u total_bytes=%llu last_pts=%llu last_pts_age_ms=%llu cur_packs=%u pending=%u source_drift_ms=%lld max_drift_ms=%lld stale_drop=%llu max_query_packs=%u pack_overflow_count=%llu",
                (unsigned long long)((now_ms - stream_start_ms) / 1000ULL),
                health_fps,
                health_kbps,
                (unsigned long long)health_interval_bytes,
                health_avg_frame_bytes,
                health_interval_max_frame_bytes,
                (unsigned long long)health_interval_pts_delta_min_us,
                (unsigned long long)health_interval_pts_delta_max_us,
                status.stream_info.u32MeanQp,
                frame_count,
                (unsigned long long)total_bytes,
                (unsigned long long)last_valid_pts,
                (unsigned long long)(now_ms - last_valid_stream_ms),
                status.cur_packs,
                stream_export_get_pending_count(),
                (long long)source_drift_ms,
                (long long)freshness.max_drift_ms,
                (unsigned long long)freshness.stale_drop_count,
                max_query_packs,
                (unsigned long long)pack_overflow_count);
            health_interval_start_ms = now_ms;
            health_interval_frames = 0;
            health_interval_bytes = 0;
            health_interval_max_frame_bytes = 0;
            health_interval_pts_delta_min_us = 0;
            health_interval_pts_delta_max_us = 0;
        }

        /* STREAM_EXPORT_LOCAL_LOG 下采集线程可能持续满速运行；主动让出调度，保证 Ctrl+C 信号路径及时执行。 */
        rt_thread_mdelay(1);

    }

    {
        double total_elapsed = (double)(venc_now_ms() - stream_start_ms) / 1000.0;

        LOG("Stream thread exit. Total: %u frames, %llu bytes in %.1fs (avg %.1f fps)",
            frame_count,
            (unsigned long long)total_bytes,
            total_elapsed,
            total_elapsed > 0 ? frame_count / total_elapsed : 0);
    }

    if (g_stream_exit_sem != RT_NULL)
        rt_sem_release(g_stream_exit_sem);
}

static k_bool wait_stream_thread_exit(k_u32 timeout_ms)
{
    k_u32 waited_ms = 0;

    if (g_stream_exit_sem == RT_NULL)
        return K_TRUE;

    while (waited_ms < timeout_ms) {
        k_s32 ret = rt_sem_take(g_stream_exit_sem,
                                rt_tick_from_millisecond(THREAD_EXIT_POLL_MS));
        if (ret == RT_EOK)
            return K_TRUE;

        waited_ms += THREAD_EXIT_POLL_MS;
    }

    LOG("Stream thread exit wait timeout after %ums", timeout_ms);
    return K_FALSE;
}

void stream_thread_stop(void)
{
    if (g_stream_tid == RT_NULL && g_stream_exit_sem == RT_NULL)
        return;

    LOG("stream_thread_stop start");
    g_stream_running = 0;

    if (g_stream_exit_sem != RT_NULL) {
        if (wait_stream_thread_exit(STREAM_THREAD_EXIT_TIMEOUT_MS)) {
            rt_sem_delete(g_stream_exit_sem);
            g_stream_exit_sem = RT_NULL;
            g_stream_tid = RT_NULL;
            LOG("stream_thread_stop done");
        } else {
            LOG("stream_thread_stop timeout; continue cleanup");
        }
    } else {
        g_stream_tid = RT_NULL;
        LOG("stream_thread_stop done");
    }
}
