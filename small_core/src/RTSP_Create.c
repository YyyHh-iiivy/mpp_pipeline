/*
 * Minimal RTSP + RTP/H.265 sender.
 *
 * File test mode:
 *   ./rtsp_sender input.h265
 *
 * DATAFIFO mode, used with teammate big-core mpp_pipeline:
 *   ./rtsp_sender --fifo 0x12ffa000
 *
 * VLC:
 *   rtsp://<board-ip>/stream
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "datafifo_snapshot.h"
#include "file.h"
#include "nalu_datafifo.h"
#include "rtp.h"
#include "rtsp.h"
#include "snapshot_writer.h"

#ifndef NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS
#define NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS 3000ULL
#endif

typedef enum {
    STREAM_SOURCE_FILE = 0,
    STREAM_SOURCE_DATAFIFO = 1
} stream_source_t;

#define H265_NAL_BLA_W_LP    16
#define H265_NAL_BLA_W_RADL  17
#define H265_NAL_BLA_N_LP    18
#define H265_NAL_IDR_W_RADL  19
#define H265_NAL_IDR_N_LP    20
#define H265_NAL_CRA_NUT     21
#define H265_NAL_VPS         32
#define H265_NAL_SPS         33
#define H265_NAL_PPS         34
#define H265_PARAM_SET_MAX_SIZE (16U * 1024U)

typedef struct {
    uint8_t *data;
    size_t len;
} h265_param_set_cache_t;

typedef struct {
    datafifo_copied_frame_t frame;
    uint8_t *storage;
    size_t storage_len;
} datafifo_owned_frame_t;

typedef struct {
    int valid;
    k_u64 base_pts_us;
    k_u64 last_pts_us;
    uint32_t base_rtp_ts;
    uint32_t last_rtp_ts;
} rtp_clock_state_t;

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

int g_play = 0;
int g_client_addr_set = 0;
int g_need_parameter_sets = 0;
unsigned int g_rtsp_session_generation = 0;
struct sockaddr_in g_client_addr;

static stream_source_t g_source = STREAM_SOURCE_FILE;
static k_u64 g_fifo_phy_addr = 0;
static k_u64 g_ctrl_fifo_phy_addr = 0;
static k_datafifo_handle g_ctrl_fifo = K_DATAFIFO_INVALID_HANDLE;
static int g_ctrl_fifo_opened = 0;
static k_u64 g_ctrl_request_seq = 0;
static uint8_t *g_file_buf = NULL;
static nalu_t *g_nalus = NULL;
static size_t g_nalu_count = 0;
static int g_snapshot_ready = 0;
static h265_param_set_cache_t g_vps_cache;
static h265_param_set_cache_t g_sps_cache;
static h265_param_set_cache_t g_pps_cache;
static unsigned int g_wait_random_access_skip_count;

#define DATAFIFO_LOG_INTERVAL 30U
#define DATAFIFO_LOW_LATENCY_MAX_AGE_MS 220ULL
#define DATAFIFO_IDR_REQUEST_INTERVAL_MS 300ULL

static int h265_have_all_parameter_sets(void);
static int ctrl_fifo_open_writer(k_u64 fifo_phy_addr);
static void ctrl_fifo_close_writer(void);
static int ctrl_fifo_request_idr(k_u64 session_generation);

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s input.h265\n", prog);
    printf("  %s --fifo <datafifo_phy_addr>\n", prog);
    printf("  %s --fifo <datafifo_phy_addr> --ctrl-fifo <ctrl_fifo_phy_addr>\n", prog);
    printf("\n");
    printf("Example:\n");
    printf("  %s girlshy.h265\n", prog);
    printf("  %s --fifo 0x12ffa000\n", prog);
    printf("\n");
    printf("DATAFIFO snapshot: set mpp_nalu_ipc_msg.reserved bit0 (0x%x).\n",
           MPP_NALU_IPC_FLAG_SNAPSHOT);
}

static void cleanup_resources(void)
{
    ctrl_fifo_close_writer();
    if (g_snapshot_ready) {
        snapshot_writer_deinit();
        g_snapshot_ready = 0;
    }
    datafifo_snapshot_deinit();
    free(g_vps_cache.data);
    free(g_sps_cache.data);
    free(g_pps_cache.data);
    memset(&g_vps_cache, 0, sizeof(g_vps_cache));
    memset(&g_sps_cache, 0, sizeof(g_sps_cache));
    memset(&g_pps_cache, 0, sizeof(g_pps_cache));
    free(g_nalus);
    g_nalus = NULL;
    free(g_file_buf);
    g_file_buf = NULL;
}

static k_u64 parse_u64_arg(const char *text)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return 0;
    }

    return (k_u64)value;
}

static int load_file_source(const char *input_path)
{
    size_t file_size = 0;
    nalu_t *nalus = NULL;
    size_t nalu_count = 0;
    uint8_t *file_buf;

    file_buf = read_whole_file(input_path, &file_size);
    if (file_buf == NULL) {
        return -1;
    }

    if (load_annexb_nalus(file_buf, file_size, &nalus, &nalu_count) != 0 ||
        nalu_count == 0) {
        printf("No Annex-B NALU found in %s\n", input_path);
        free(file_buf);
        return -1;
    }

    g_file_buf = file_buf;
    g_nalus = nalus;
    g_nalu_count = nalu_count;

    printf("[source] file=%s size=%lu nalus=%lu\n",
           input_path,
           (unsigned long)file_size,
           (unsigned long)nalu_count);

    return 0;
}

static int parse_args(int argc, char **argv)
{
    const char *input_path = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fifo") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            g_source = STREAM_SOURCE_DATAFIFO;
            g_fifo_phy_addr = parse_u64_arg(argv[++i]);
            if (g_fifo_phy_addr == 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ctrl-fifo") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            g_ctrl_fifo_phy_addr = parse_u64_arg(argv[++i]);
            if (g_ctrl_fifo_phy_addr == 0) {
                return -1;
            }
        } else if (argv[i][0] == '-') {
            return -1;
        } else {
            if (input_path != NULL) {
                return -1;
            }
            input_path = argv[i];
        }
    }

    if (g_source == STREAM_SOURCE_DATAFIFO) {
        if (input_path != NULL) {
            return -1;
        }
        printf("[source] DATAFIFO mode, fifo phy=0x%llx\n",
               (unsigned long long)g_fifo_phy_addr);
        if (g_ctrl_fifo_phy_addr != 0) {
            printf("[source] CTRL FIFO phy=0x%llx\n",
                   (unsigned long long)g_ctrl_fifo_phy_addr);
        }
        return 0;
    }

    if (input_path == NULL) {
        return -1;
    }

    g_source = STREAM_SOURCE_FILE;
    return load_file_source(input_path);
}
static uint64_t monotonic_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }

    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000U);
}

static int ctrl_fifo_open_writer(k_u64 fifo_phy_addr)
{
    k_s32 ret;
    k_datafifo_params_s params;

    if (fifo_phy_addr == 0) {
        return 0;
    }

    memset(&params, 0, sizeof(params));
    params.u32EntriesNum = MPP_CTRL_IPC_ENTRIES;
    params.u32CacheLineSize = MPP_CTRL_IPC_ITEM_SIZE;
    params.bDataReleaseByWriter = K_FALSE;
    params.enOpenMode = DATAFIFO_WRITER;

    ret = kd_datafifo_open_by_addr(&g_ctrl_fifo, &params, fifo_phy_addr);
    if (ret != 0) {
        printf("[ctrl] open writer failed phy=0x%llx ret=0x%x\n",
               (unsigned long long)fifo_phy_addr,
               ret);
        g_ctrl_fifo = K_DATAFIFO_INVALID_HANDLE;
        return -1;
    }

    g_ctrl_fifo_opened = 1;
    printf("[ctrl] opened writer phy=0x%llx item=%u entries=%u\n",
           (unsigned long long)fifo_phy_addr,
           (unsigned int)MPP_CTRL_IPC_ITEM_SIZE,
           (unsigned int)MPP_CTRL_IPC_ENTRIES);
    return 0;
}

static void ctrl_fifo_close_writer(void)
{
    if (!g_ctrl_fifo_opened)
        return;

    kd_datafifo_close(g_ctrl_fifo);
    g_ctrl_fifo = K_DATAFIFO_INVALID_HANDLE;
    g_ctrl_fifo_opened = 0;
}

static int ctrl_fifo_request_idr(k_u64 session_generation)
{
    k_s32 ret;
    k_u32 avail_write_len = 0;
    mpp_ctrl_ipc_msg msg;

    if (!g_ctrl_fifo_opened)
        return 0;

    ret = kd_datafifo_write(g_ctrl_fifo, NULL);
    if (ret != 0) {
        printf("[ctrl] flush writer failed ret=0x%x\n", ret);
        return -1;
    }

    ret = kd_datafifo_cmd(g_ctrl_fifo,
                          DATAFIFO_CMD_GET_AVAIL_WRITE_LEN,
                          &avail_write_len);
    if (ret != 0 || avail_write_len < MPP_CTRL_IPC_ITEM_SIZE) {
        printf("[ctrl] request IDR drop: ret=0x%x avail=%u need=%u\n",
               ret,
               (unsigned int)avail_write_len,
               (unsigned int)MPP_CTRL_IPC_ITEM_SIZE);
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    msg.magic = MPP_CTRL_IPC_MAGIC;
    msg.version = MPP_CTRL_IPC_VERSION;
    msg.cmd = MPP_CTRL_CMD_REQUEST_IDR;
    msg.seq = ++g_ctrl_request_seq;
    msg.session_generation = session_generation;
    msg.request_time_ms = monotonic_ms();

    ret = kd_datafifo_write(g_ctrl_fifo, &msg);
    if (ret != 0) {
        printf("[ctrl] request IDR write failed ret=0x%x seq=%llu\n",
               ret,
               (unsigned long long)msg.seq);
        return -1;
    }

    ret = kd_datafifo_cmd(g_ctrl_fifo, DATAFIFO_CMD_WRITE_DONE, NULL);
    if (ret != 0) {
        printf("[ctrl] request IDR write_done failed ret=0x%x seq=%llu\n",
               ret,
               (unsigned long long)msg.seq);
        return -1;
    }

    printf("[ctrl] request IDR sent seq=%llu session=%llu t=%llu\n",
           (unsigned long long)msg.seq,
           (unsigned long long)msg.session_generation,
           (unsigned long long)msg.request_time_ms);
    return 0;
}

static int ctrl_fifo_request_idr_rate_limited(k_u64 session_generation,
                                              const char *reason)
{
    static uint64_t last_request_ms;
    static int missing_ctrl_fifo_logged;
    uint64_t now_ms = monotonic_ms();

    if (!g_ctrl_fifo_opened) {
        if (!missing_ctrl_fifo_logged) {
            printf("[ctrl] CTRL FIFO not open, cannot request IDR for %s; pass --ctrl-fifo <phy>\n",
                   reason ? reason : "low-latency recovery");
            missing_ctrl_fifo_logged = 1;
        }
        return 0;
    }

    if (now_ms != 0 &&
        last_request_ms != 0 &&
        now_ms - last_request_ms < DATAFIFO_IDR_REQUEST_INTERVAL_MS) {
        return 0;
    }

    if (now_ms != 0) {
        last_request_ms = now_ms;
    }

    printf("[ctrl] request IDR reason=%s session=%llu\n",
           reason ? reason : "low-latency recovery",
           (unsigned long long)session_generation);
    return ctrl_fifo_request_idr(session_generation);
}

static void rtp_clock_reset(rtp_clock_state_t *clock_state, uint32_t base_ts)
{
    if (clock_state == NULL)
        return;

    memset(clock_state, 0, sizeof(*clock_state));
    clock_state->base_rtp_ts = base_ts;
    clock_state->last_rtp_ts = base_ts;
}

static uint32_t rtp_clock_next_timestamp(rtp_clock_state_t *clock_state,
                                         k_u64 frame_pts_us,
                                         int *pts_mode,
                                         k_u64 *pts_delta_us,
                                         uint32_t *rtp_delta)
{
    uint32_t next_ts;
    int use_pts = 0;

    if (pts_mode)
        *pts_mode = 0;
    if (pts_delta_us)
        *pts_delta_us = 0;
    if (rtp_delta)
        *rtp_delta = 0;

    if (clock_state == NULL) {
        return 0x12345678;
    }

    next_ts = clock_state->last_rtp_ts + RTP_TS_STEP;
    if (frame_pts_us != 0) {
        if (!clock_state->valid) {
            clock_state->valid = 1;
            clock_state->base_pts_us = frame_pts_us;
            clock_state->last_pts_us = frame_pts_us;
            next_ts = clock_state->base_rtp_ts;
            use_pts = 1;
        } else if (frame_pts_us > clock_state->last_pts_us &&
                   frame_pts_us - clock_state->last_pts_us <= 1000000ULL) {
            k_u64 rel_us = frame_pts_us - clock_state->base_pts_us;
            k_u64 delta_us = frame_pts_us - clock_state->last_pts_us;

            next_ts = clock_state->base_rtp_ts +
                      (uint32_t)((rel_us * RTP_CLOCK_RATE) / 1000000ULL);
            if (next_ts <= clock_state->last_rtp_ts) {
                next_ts = clock_state->last_rtp_ts + RTP_TS_STEP;
            }
            if (pts_delta_us)
                *pts_delta_us = delta_us;
            use_pts = 1;
        }
    }

    if (rtp_delta)
        *rtp_delta = next_ts - clock_state->last_rtp_ts;
    if (pts_mode)
        *pts_mode = use_pts;

    if (use_pts && frame_pts_us != 0)
        clock_state->last_pts_us = frame_pts_us;
    clock_state->last_rtp_ts = next_ts;
    return next_ts;
}

static void datafifo_log_reader_idle(uint64_t last_seq,
                                     uint64_t last_item_ms,
                                     unsigned int idle_loops)
{
    static uint64_t last_idle_log_ms;
    uint64_t now = monotonic_ms();

    if (now == 0) {
        return;
    }

    if (last_idle_log_ms == 0 ||
        now - last_idle_log_ms >= NALU_DATAFIFO_IDLE_LOG_INTERVAL_MS) {
        uint64_t idle_ms = last_item_ms ? now - last_item_ms : 0;

        printf("[datafifo] reader idle last_seq=%llu idle_ms=%llu idle_loops=%u\n",
               (unsigned long long)last_seq,
               (unsigned long long)idle_ms,
               idle_loops);
        last_idle_log_ms = now;
    }
}

static void log_datafifo_frame_stats(uint64_t seq,
                                     uint32_t pack_cnt,
                                     uint32_t total_len,
                                     uint64_t pts,
                                     uint64_t submit_time_ms,
                                     uint64_t age_ms,
                                     uint32_t flags,
                                     int playing,
                                     uint64_t last_seq,
                                     uint64_t send_cost_ms,
                                     uint64_t pts_delta_us,
                                     uint32_t rtp_delta,
                                     uint32_t rtp_ts,
                                     int pts_mode,
                                     unsigned int frame_count,
                                     int force)
{
    uint64_t seq_gap = 0;

    if (last_seq != 0 && seq > last_seq + 1ULL) {
        seq_gap = seq - last_seq - 1ULL;
    }

    if (!force && seq_gap == 0 && (frame_count % DATAFIFO_LOG_INTERVAL) != 0) {
        return;
    }

    printf("[datafifo] frame seq=%llu gap=%llu packs=%u total=%u pts=%llu pts_delta_us=%llu rtp_ts=%u rtp_delta=%u pts_mode=%d submit=%llu age=%llums send=%llums play=%d flags=0x%x\n",
           (unsigned long long)seq,
           (unsigned long long)seq_gap,
           pack_cnt,
           total_len,
           (unsigned long long)pts,
           (unsigned long long)pts_delta_us,
           (unsigned int)rtp_ts,
           (unsigned int)rtp_delta,
           pts_mode,
           (unsigned long long)submit_time_ms,
           (unsigned long long)age_ms,
           (unsigned long long)send_cost_ms,
           playing,
           flags);
}

static void datafifo_owned_frame_free(datafifo_owned_frame_t *owned)
{
    if (owned == NULL) {
        return;
    }

    free(owned->storage);
    memset(owned, 0, sizeof(*owned));
}
// 复制消息中的数据包到所有者帧中
static int datafifo_copy_msg_packs(const mpp_nalu_ipc_msg *msg,
                                   datafifo_owned_frame_t *owned)
{
    uint8_t *write_ptr;
    size_t copy_len = 0;
    k_u32 i;

    if (msg == NULL || owned == NULL || msg->pack_cnt == 0 ||
        msg->pack_cnt > MPP_NALU_IPC_MAX_PACKS) {
        return -1;
    }

    for (i = 0; i < msg->pack_cnt; i++) {
        copy_len += msg->packs[i].len;
    }

    if (copy_len == 0) {
        return -1;
    }

    memset(owned, 0, sizeof(*owned));
    owned->storage = (uint8_t *)malloc(copy_len);
    if (owned->storage == NULL) {
        printf("[datafifo] copy alloc failed seq=%llu total=%lu\n",
               (unsigned long long)msg->seq,
               (unsigned long)copy_len);
        return -1;
    }

    owned->storage_len = copy_len;
    owned->frame.chn = msg->chn;
    owned->frame.pack_cnt = msg->pack_cnt;
    owned->frame.seq = msg->seq;
    owned->frame.frame_pts = msg->frame_pts;
    owned->frame.submit_time_ms = msg->submit_time_ms;
    owned->frame.total_len = (k_u32)copy_len;
    owned->frame.reserved = msg->reserved;

    write_ptr = owned->storage;
    for (i = 0; i < msg->pack_cnt; i++) {
        void *virt_addr;

        virt_addr = nalu_datafifo_mmap_pack(&msg->packs[i]);
        if (virt_addr == NULL) {
            printf("[datafifo] copy mmap failed seq=%llu pack[%u] phys=0x%llx len=%u\n",
                   (unsigned long long)msg->seq,
                   i,
                   (unsigned long long)msg->packs[i].phys_addr,
                   msg->packs[i].len);
            datafifo_owned_frame_free(owned);
            return -1;
        }

        memcpy(write_ptr, virt_addr, msg->packs[i].len);
        owned->frame.packs[i].data = write_ptr;
        owned->frame.packs[i].len = msg->packs[i].len;
        owned->frame.packs[i].pts = msg->packs[i].pts;
        owned->frame.packs[i].type = msg->packs[i].type;
        write_ptr += msg->packs[i].len;

        if (nalu_datafifo_munmap_pack(&msg->packs[i], virt_addr) != 0) {
            printf("[datafifo] copy munmap failed seq=%llu pack[%u] phys=0x%llx len=%u\n",
                   (unsigned long long)msg->seq,
                   i,
                   (unsigned long long)msg->packs[i].phys_addr,
                   msg->packs[i].len);
            datafifo_owned_frame_free(owned);
            return -1;
        }
    }

    return 0;
}

static int datafifo_should_copy_frame(int can_send,
                                      int need_parameter_sets,
                                      const mpp_nalu_ipc_msg *msg)
{
    if (msg == NULL) {
        return 0;
    }
    (void)need_parameter_sets;

    return can_send ||
           !h265_have_all_parameter_sets() ||
           ((msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT) != 0);
}

static void log_datafifo_read_done(uint64_t seq,
                                   uint64_t read_done_seq,
                                   uint64_t read_done_fail_count,
                                   int read_done_ret,
                                   uint64_t copy_cost_ms,
                                   uint64_t read_done_cost_ms,
                                   uint64_t pre_done_cost_ms,
                                   uint32_t total_len,
                                   uint32_t flags,
                                   int force)
{
    if (!force && read_done_ret == 0 && (read_done_seq % DATAFIFO_LOG_INTERVAL) != 0) {
        return;
    }

    printf("[datafifo] read_done seq=%llu read_done_seq=%llu read_done_fail=%llu ret=%d copy=%llums read_done=%llums pre_done=%llums total=%u flags=0x%x\n",
           (unsigned long long)seq,
           (unsigned long long)read_done_seq,
           (unsigned long long)read_done_fail_count,
           read_done_ret,
           (unsigned long long)copy_cost_ms,
           (unsigned long long)read_done_cost_ms,
           (unsigned long long)pre_done_cost_ms,
           total_len,
           flags);
}

static long h265_find_start_code_local(const uint8_t *buf,
                                       size_t size,
                                       size_t offset,
                                       size_t *start_code_len)
{
    size_t i;

    for (i = offset; i + 3U <= size; i++) {
        if (buf[i] == 0x00 && buf[i + 1U] == 0x00 && buf[i + 2U] == 0x01) {
            *start_code_len = 3U;
            return (long)i;
        }

        if (i + 4U <= size &&
            buf[i] == 0x00 &&
            buf[i + 1U] == 0x00 &&
            buf[i + 2U] == 0x00 &&
            buf[i + 3U] == 0x01) {
            *start_code_len = 4U;
            return (long)i;
        }
    }

    return -1;
}

static int h265_buffer_starts_with_start_code_local(const uint8_t *buf,
                                                    size_t len)
{
    if (buf == NULL || len < 3U) {
        return 0;
    }

    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) {
        return 1;
    }

    return len >= 4U &&
           buf[0] == 0x00 &&
           buf[1] == 0x00 &&
           buf[2] == 0x00 &&
           buf[3] == 0x01;
}

static int h265_is_parameter_set(int nal_type)
{
    return nal_type == H265_NAL_VPS ||
           nal_type == H265_NAL_SPS ||
           nal_type == H265_NAL_PPS;
}

static int h265_is_idr(int nal_type)
{
    return nal_type == H265_NAL_IDR_W_RADL ||
           nal_type == H265_NAL_IDR_N_LP;
}

static int h265_is_random_access(int nal_type)
{
    return nal_type == H265_NAL_BLA_W_LP ||
           nal_type == H265_NAL_BLA_W_RADL ||
           nal_type == H265_NAL_BLA_N_LP ||
           h265_is_idr(nal_type) ||
           nal_type == H265_NAL_CRA_NUT;
}

static int h265_is_vcl(int nal_type)
{
    return nal_type >= 0 && nal_type <= 31;
}

static const char *h265_nal_name(int nal_type)
{
    switch (nal_type) {
    case H265_NAL_BLA_W_LP:
        return "BLA_W_LP";
    case H265_NAL_BLA_W_RADL:
        return "BLA_W_RADL";
    case H265_NAL_BLA_N_LP:
        return "BLA_N_LP";
    case H265_NAL_VPS:
        return "VPS";
    case H265_NAL_SPS:
        return "SPS";
    case H265_NAL_PPS:
        return "PPS";
    case H265_NAL_IDR_W_RADL:
        return "IDR_W_RADL";
    case H265_NAL_IDR_N_LP:
        return "IDR_N_LP";
    case H265_NAL_CRA_NUT:
        return "CRA";
    default:
        return "NAL";
    }
}

static h265_param_set_cache_t *h265_param_cache_for_type(int nal_type)
{
    if (nal_type == H265_NAL_VPS) {
        return &g_vps_cache;
    }
    if (nal_type == H265_NAL_SPS) {
        return &g_sps_cache;
    }
    if (nal_type == H265_NAL_PPS) {
        return &g_pps_cache;
    }

    return NULL;
}

static int h265_have_all_parameter_sets(void)
{
    return g_vps_cache.data != NULL && g_vps_cache.len > 0 &&
           g_sps_cache.data != NULL && g_sps_cache.len > 0 &&
           g_pps_cache.data != NULL && g_pps_cache.len > 0;
}

static int h265_cache_parameter_set(const uint8_t *nalu, size_t nalu_len)
{
    int nal_type;
    uint8_t *copy;
    h265_param_set_cache_t *cache;

    if (nalu == NULL || nalu_len < 2U) {
        return 0;
    }

    nal_type = h265_nalu_type(nalu, nalu_len);
    cache = h265_param_cache_for_type(nal_type);
    if (cache == NULL) {
        return 0;
    }

    if (nalu_len > H265_PARAM_SET_MAX_SIZE) {
        printf("[h265] skip oversized %s parameter set len=%lu\n",
               h265_nal_name(nal_type),
               (unsigned long)nalu_len);
        return -1;
    }

    if (cache->data != NULL &&
        cache->len == nalu_len &&
        memcmp(cache->data, nalu, nalu_len) == 0) {
        return 0;
    }

    copy = (uint8_t *)malloc(nalu_len);
    if (copy == NULL) {
        printf("[h265] malloc failed for %s len=%lu\n",
               h265_nal_name(nal_type),
               (unsigned long)nalu_len);
        return -1;
    }

    memcpy(copy, nalu, nalu_len);
    free(cache->data);
    cache->data = copy;
    cache->len = nalu_len;

    printf("[h265] cached %s len=%lu\n",
           h265_nal_name(nal_type),
           (unsigned long)nalu_len);

    return 0;
}

static int send_cached_parameter_sets(int sock,
                                      const struct sockaddr_in *dest,
                                      uint16_t *seq,
                                      uint32_t timestamp,
                                      uint32_t ssrc)
{
    static unsigned int missing_log_count = 0;

    if (!h265_have_all_parameter_sets()) {
        missing_log_count++;
        if (missing_log_count == 1U || (missing_log_count % 30U) == 0U) {
            printf("[rtp] waiting for VPS/SPS/PPS before PLAY: vps=%lu sps=%lu pps=%lu\n",
                   (unsigned long)g_vps_cache.len,
                   (unsigned long)g_sps_cache.len,
                   (unsigned long)g_pps_cache.len);
        }
        return -1;
    }

    if (send_h265_nalu_rtp(sock, dest, g_vps_cache.data, g_vps_cache.len,
                           seq, timestamp, ssrc, 0) != 0 ||
        send_h265_nalu_rtp(sock, dest, g_sps_cache.data, g_sps_cache.len,
                           seq, timestamp, ssrc, 0) != 0 ||
        send_h265_nalu_rtp(sock, dest, g_pps_cache.data, g_pps_cache.len,
                           seq, timestamp, ssrc, 0) != 0) {
        return -1;
    }

    printf("[rtp] sent cached VPS/SPS/PPS ts=%lu seq=%u -> %s:%d\n",
           (unsigned long)timestamp,
           (unsigned int)*seq,
           inet_ntoa(dest->sin_addr),
           ntohs(dest->sin_port));

    return 0;
}
// 处理DATAFIFO NALU
static int datafifo_process_nalu_for_playback(int sock,
                                              const struct sockaddr_in *dest,
                                              const uint8_t *nalu,
                                              size_t nalu_len,
                                              uint16_t *seq,
                                              uint32_t timestamp,
                                              uint32_t ssrc,
                                              uint8_t marker,
                                              int can_send,
                                              int *need_parameter_sets,
                                              int *wait_for_idr)
{
    int nal_type;
    int startup_random_access = 0;
    int send_ret;

    if (nalu == NULL || nalu_len < 2U) {
        return 0;
    }
// 检查NALU类型
    nal_type = h265_nalu_type(nalu, nalu_len);
    if (h265_is_parameter_set(nal_type)) {
        h265_cache_parameter_set(nalu, nalu_len);
    }

    if (!can_send) {
        return 0;
    }

    if (*wait_for_idr) {
        if (!h265_is_random_access(nal_type)) {
            if (h265_is_vcl(nal_type)) {
                g_wait_random_access_skip_count++;
                if (g_wait_random_access_skip_count == 1U ||
                    (g_wait_random_access_skip_count % 30U) == 0U) {
                    printf("[rtp] waiting random access, skip vcl count=%u type=%d(%s)\n",
                           g_wait_random_access_skip_count,
                           nal_type,
                           h265_nal_name(nal_type));
                }
            }
            return 0;
        }

        rtp_set_packet_duplicate(1);
        if (send_cached_parameter_sets(sock, dest, seq, timestamp, ssrc) != 0) {
            rtp_set_packet_duplicate(0);
            return 0;
        }
        if (need_parameter_sets)
            *need_parameter_sets = 0;
        *wait_for_idr = 0;
        startup_random_access = 1;
        printf("[rtp] random access start: skipped_vcl=%u type=%d(%s) len=%lu\n",
               g_wait_random_access_skip_count,
               nal_type,
               h265_nal_name(nal_type),
               (unsigned long)nalu_len);
        g_wait_random_access_skip_count = 0;
    } else if (need_parameter_sets && *need_parameter_sets) {
        *wait_for_idr = 1;
        printf("[rtp] parameter sets requested, wait random access\n");
        return 0;
    }

    send_ret = send_h265_nalu_rtp(sock,
                                  dest,
                                  nalu,
                                  nalu_len,
                                  seq,
                                  timestamp,
                                  ssrc,
                                  marker);
    if (startup_random_access) {
        rtp_set_packet_duplicate(0);
    }
    return send_ret;
}
// 处理H265缓冲区
static int datafifo_process_h265_buffer_for_playback(int sock,
                                                     const struct sockaddr_in *dest,
                                                     const uint8_t *buf,
                                                     size_t len,
                                                     uint16_t *seq,
                                                     uint32_t timestamp,
                                                     uint32_t ssrc,
                                                     uint8_t last_marker,
                                                     int can_send,
                                                     int *need_parameter_sets,
                                                     int *wait_for_idr)
{
    size_t search_offset = 0;

    if (buf == NULL || len < 2U) {
        return 0;
    }

    if (!h265_buffer_starts_with_start_code_local(buf, len)) {
        return datafifo_process_nalu_for_playback(sock,
                                                  dest,
                                                  buf,
                                                  len,
                                                  seq,
                                                  timestamp,
                                                  ssrc,
                                                  last_marker,
                                                  can_send,
                                                  need_parameter_sets,
                                                  wait_for_idr);
    }

    while (1) {
        size_t sc_len = 0;
        size_t next_sc_len = 0;
        long sc_pos;
        long next_sc_pos;
        size_t nalu_start;
        size_t nalu_end;
        uint8_t marker;

        sc_pos = h265_find_start_code_local(buf, len, search_offset, &sc_len);
        if (sc_pos < 0) {
            break;
        }

        nalu_start = (size_t)sc_pos + sc_len;
        next_sc_pos = h265_find_start_code_local(buf, len, nalu_start, &next_sc_len);
        nalu_end = (next_sc_pos < 0) ? len : (size_t)next_sc_pos;

        while (nalu_end > nalu_start && buf[nalu_end - 1U] == 0x00) {
            nalu_end--;
        }

        if (nalu_end > nalu_start) {
            marker = (next_sc_pos < 0) ? last_marker : 0;
            if (datafifo_process_nalu_for_playback(sock,
                                                   dest,
                                                   buf + nalu_start,
                                                   nalu_end - nalu_start,
                                                   seq,
                                                   timestamp,
                                                   ssrc,
                                                   marker,
                                                   can_send,
                                                   need_parameter_sets,
                                                   wait_for_idr) != 0) {
                return -1;
            }
        }

        if (next_sc_pos < 0) {
            break;
        }
        search_offset = (size_t)next_sc_pos;
    }

    return 0;
}
// 处理DATAFIFO复制的帧
static int datafifo_process_copied_frame_for_playback(int sock,
                                                      const struct sockaddr_in *dest,
                                                      const datafifo_copied_frame_t *frame,
                                                      uint16_t *seq,
                                                      uint32_t timestamp,
                                                      uint32_t ssrc,
                                                      int can_send,
                                                      int *need_parameter_sets,
                                                      int *wait_for_idr)
{
    k_u32 i;

    if (frame == NULL) {
        return -1;
    }

    for (i = 0; i < frame->pack_cnt; i++) {
        uint8_t marker = (i + 1U == frame->pack_cnt) ? 1 : 0;

        if (datafifo_process_h265_buffer_for_playback(sock,
                                                      dest,
                                                      frame->packs[i].data,
                                                      frame->packs[i].len,
                                                      seq,
                                                      timestamp,
                                                      ssrc,
                                                      marker,
                                                      can_send,
                                                      need_parameter_sets,
                                                      wait_for_idr) != 0) {
            printf("[datafifo] process copied pack[%u] failed seq=%llu len=%lu\n",
                   i,
                   (unsigned long long)frame->seq,
                   (unsigned long)frame->packs[i].len);
            return -1;
        }
    }

    return 0;
}

// 文件 RTP发送线程
static void *file_rtp_sender_loop(int sock)
{
    typedef struct {
        int active;
        unsigned int generation;
        uint16_t seq;
    } file_target_state_t;
    file_target_state_t states[RTSP_MAX_CLIENTS];
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;
    size_t idx = 0;

    memset(states, 0, sizeof(states));

    while (1) {
        rtsp_target_t targets[RTSP_MAX_CLIENTS];
        int target_count;
        const nalu_t *nalu;
        int nal_type;

        target_count = rtsp_get_targets(targets, RTSP_MAX_CLIENTS);
        if (target_count == 0) {
            usleep(SEND_INTERVAL_US);
            continue;
        }

        if (g_nalu_count == 0) {
            usleep(SEND_INTERVAL_US);
            continue;
        }

        nalu = &g_nalus[idx];
        nal_type = h265_nalu_type(nalu->data, nalu->len);

        for (int i = 0; i < target_count; i++) {
            int slot = targets[i].slot;
            if (slot < 0 || slot >= RTSP_MAX_CLIENTS)
                continue;
            if (!states[slot].active || states[slot].generation != targets[i].generation) {
                states[slot].active = 1;
                states[slot].generation = targets[i].generation;
                states[slot].seq = 0;
            }
            (void)send_h265_nalu_rtp(sock,
                                     &targets[i].rtp_addr,
                                     nalu->data,
                                     nalu->len,
                                     &states[slot].seq,
                                     timestamp,
                                     ssrc,
                                     1);
        }

        printf("[rtp:file] nalu=%lu type=%d len=%lu ts=%lu seq=%u -> %s:%d\n",
               (unsigned long)idx,
               nal_type,
               (unsigned long)nalu->len,
               (unsigned long)timestamp,
               (unsigned int)states[targets[0].slot].seq,
               inet_ntoa(targets[0].rtp_addr.sin_addr),
               ntohs(targets[0].rtp_addr.sin_port));

        idx++;
        if (idx >= g_nalu_count) {
            idx = 0;
        }

        timestamp += RTP_TS_STEP;
        usleep(SEND_INTERVAL_US);
    }

    return NULL;
}
// DATAFIFO RTP发送线程
static void *datafifo_rtp_sender_loop(int sock)
{
    typedef struct {
        int active;
        unsigned int generation;
        uint16_t seq;
        int wait_for_idr;
        uint64_t last_sent_seq;
    } sender_target_state_t;
    nalu_datafifo_reader_t reader;
    uint32_t timestamp = 0x12345678;
    uint32_t ssrc = 0x22334455;
    rtp_clock_state_t rtp_clock;
    uint64_t last_sent_seq = 0;
    uint64_t last_read_seq = 0;
    uint64_t last_item_seq = 0;
    uint64_t last_item_ms = monotonic_ms();
    unsigned int frame_count = 0;
    unsigned int idle_loops = 0;
    uint64_t read_done_seq = 0;
    uint64_t read_done_fail_count = 0;
    uint64_t dropped_backlog_count = 0;
    uint64_t rtp_drop_count = 0;
    sender_target_state_t target_states[RTSP_MAX_CLIENTS];

    memset(target_states, 0, sizeof(target_states));

    if (nalu_datafifo_open(&reader, g_fifo_phy_addr) != 0) {
        printf("[datafifo] open failed, RTP sender stopped\n");
        return NULL;
    }
    rtp_clock_reset(&rtp_clock, timestamp);

    while (1) {
        const mpp_nalu_ipc_msg *msg = NULL;
        void *item = NULL;
        rtsp_target_t targets[RTSP_MAX_CLIENTS];
        int target_count;
        int playing = 0;
        int dest_valid = 0;
        int need_parameter_sets = 0;
        int can_send = 0;
        int valid_msg = 0;
        int stale_frame = 0;
        int late_frame = 0;
        int should_copy = 0;
        int copy_ret = -1;
        int snapshot_ret = 0;
        int send_failed = 0;
        int read_done_ret = -1;
        uint64_t msg_seq = 0;
        uint32_t msg_total_len = 0;
        uint32_t msg_flags = 0;
        uint64_t msg_age_ms = 0;
        uint64_t copy_start_ms = 0;
        uint64_t copy_cost_ms = 0;
        uint64_t pre_done_cost_ms = 0;
        uint64_t read_done_start_ms = 0;
        uint64_t read_done_cost_ms = 0;
        uint64_t send_start_ms = 0;
        uint64_t send_cost_ms = 0;
        uint64_t prev_sent_seq = 0;
        k_u32 avail_read_len = 0;
        k_u32 drop_budget = 0;
        k_u32 dropped_this_round = 0;
        unsigned int session_generation = 0;
        int send_ret = RTP_SEND_OK;
        int pts_mode = 0;
        uint64_t pts_delta_us = 0;
        uint32_t rtp_delta = 0;
        uint32_t frame_rtp_ts = timestamp;
        datafifo_owned_frame_t owned_frame;

        /*
         * Low-latency policy: after the RTSP session has valid parameter
         * sets, discard queued old DATAFIFO items and keep the newest item
         * from this backlog snapshot. Every discarded item is READ_DONE
         * immediately so the big core can release its VENC buffer.
         */
        target_count = rtsp_get_targets(targets, RTSP_MAX_CLIENTS);
        playing = target_count > 0;
        dest_valid = playing;
        can_send = playing;
        if (can_send) {
            need_parameter_sets = targets[0].need_parameter_sets;
            session_generation = targets[0].generation;
        }
        for (int ti = 0; ti < target_count; ti++) {
            int slot = targets[ti].slot;
            if (slot < 0 || slot >= RTSP_MAX_CLIENTS)
                continue;
            if (!target_states[slot].active ||
                target_states[slot].generation != targets[ti].generation) {
                target_states[slot].active = 1;
                target_states[slot].generation = targets[ti].generation;
                target_states[slot].seq = 0;
                target_states[slot].wait_for_idr = 1;
                target_states[slot].last_sent_seq = 0;
                rtsp_target_request_parameter_sets(slot);
                (void)ctrl_fifo_request_idr(targets[ti].generation);
                printf("[rtp] new client slot=%d generation=%u, wait random access\n",
                       slot, targets[ti].generation);
            }
        }
        if (can_send &&
            h265_have_all_parameter_sets() &&
            nalu_datafifo_get_avail_read_len(&reader, &avail_read_len) == 0 &&
            avail_read_len >= 2U * MPP_NALU_IPC_ITEM_SIZE) {
            drop_budget = avail_read_len / MPP_NALU_IPC_ITEM_SIZE - 1U;
        }

        while (drop_budget > 0) {
            const mpp_nalu_ipc_msg *old_msg = NULL;
            void *old_item = NULL;
            int old_valid;
            uint64_t old_seq = 0;

            if (nalu_datafifo_read(&reader, &old_msg, &old_item) != 0) {
                break;
            }

            old_valid = nalu_datafifo_validate_msg(old_msg) == 0;
            if (old_valid) {
                old_seq = old_msg->seq;
            }
            if (old_valid &&
                (old_msg->reserved & MPP_NALU_IPC_FLAG_SNAPSHOT) != 0) {
                msg = old_msg;
                item = old_item;
                break;
            }

            if (nalu_datafifo_read_done(&reader, old_item) != 0) {
                /* Keep ownership; the normal path will retry READ_DONE once. */
                msg = old_msg;
                item = old_item;
                break;
            }

            read_done_seq = old_valid ? old_seq : read_done_seq;
            dropped_this_round++;
            dropped_backlog_count++;
            drop_budget--;
        }

        if (item == NULL &&
            nalu_datafifo_read(&reader, &msg, &item) != 0) {
            idle_loops++;
            datafifo_log_reader_idle(last_item_seq, last_item_ms, idle_loops);
            usleep(NALU_DATAFIFO_READ_IDLE_US);
            continue;
        }

        if (dropped_this_round > 0) {
            /* Dropped reference pictures: resume RTP from a clean IDR. */
            for (int ti = 0; ti < target_count; ti++) {
                int slot = targets[ti].slot;
                if (slot >= 0 && slot < RTSP_MAX_CLIENTS) {
                    target_states[slot].wait_for_idr = 1;
                    rtsp_target_request_parameter_sets(slot);
                }
            }
            (void)ctrl_fifo_request_idr_rate_limited(session_generation,
                                                     "backlog_drop");
            if (dropped_backlog_count == dropped_this_round ||
                (dropped_backlog_count % DATAFIFO_LOG_INTERVAL) == 0) {
                printf("[datafifo] low-latency drop old=%u total_drop=%llu, wait for IDR\n",
                       dropped_this_round,
                       (unsigned long long)dropped_backlog_count);
            }
        }

        idle_loops = 0;
        if (msg != NULL) {
            last_item_seq = msg->seq;
        }
        last_item_ms = monotonic_ms();

        if (nalu_datafifo_validate_msg(msg) == 0) {
            valid_msg = 1;
            msg_seq = msg->seq;
            msg_total_len = msg->total_len;
            msg_flags = msg->reserved;

            if (msg->submit_time_ms != 0 &&
                last_item_ms >= msg->submit_time_ms) {
                msg_age_ms = last_item_ms - msg->submit_time_ms;
            }

            if (can_send &&
                h265_have_all_parameter_sets() &&
                msg_age_ms > DATAFIFO_LOW_LATENCY_MAX_AGE_MS &&
                (msg_flags & MPP_NALU_IPC_FLAG_SNAPSHOT) == 0) {
                late_frame = 1;
                for (int ti = 0; ti < target_count; ti++) {
                    int slot = targets[ti].slot;
                    if (slot >= 0 && slot < RTSP_MAX_CLIENTS) {
                        target_states[slot].wait_for_idr = 1;
                        rtsp_target_request_parameter_sets(slot);
                    }
                }
                (void)ctrl_fifo_request_idr_rate_limited(session_generation,
                                                         "late_frame");
            }

            if (last_read_seq != 0 && msg->seq <= last_read_seq) {
                stale_frame = 1;
                printf("[datafifo] seq rollback/stale current=%llu last=%llu flags=0x%x item=%p\n",
                       (unsigned long long)msg->seq,
                       (unsigned long long)last_read_seq,
                       msg->reserved,
                       item);
            }
            if (!stale_frame) {
                last_read_seq = msg->seq;
            }

            should_copy = !stale_frame &&
                          !late_frame &&
                          datafifo_should_copy_frame(can_send, need_parameter_sets, msg);
            if (should_copy) {
                copy_start_ms = monotonic_ms();
                copy_ret = datafifo_copy_msg_packs(msg, &owned_frame);
                copy_cost_ms = monotonic_ms() - copy_start_ms;
            }

            if (stale_frame) {
                printf("[datafifo] drop stale seq=%llu last=%llu, READ_DONE only\n",
                       (unsigned long long)msg->seq,
                       (unsigned long long)last_read_seq);
            } else if (late_frame) {
                printf("[datafifo] drop late seq=%llu age=%llums threshold=%llums total=%u flags=0x%x, wait for IDR\n",
                       (unsigned long long)msg->seq,
                       (unsigned long long)msg_age_ms,
                       (unsigned long long)DATAFIFO_LOW_LATENCY_MAX_AGE_MS,
                       msg_total_len,
                       msg_flags);
            } else if (should_copy && copy_ret != 0) {
                printf("[datafifo] copy failed seq=%llu, READ_DONE before drop\n",
                       (unsigned long long)msg->seq);
            }
        } else {
            printf("[datafifo] skip invalid item but still READ_DONE item=%p\n",
                   item);
        }

        pre_done_cost_ms = monotonic_ms() - last_item_ms;
        read_done_start_ms = monotonic_ms();
        read_done_ret = nalu_datafifo_read_done(&reader, item);
        read_done_cost_ms = monotonic_ms() - read_done_start_ms;
        if (read_done_ret != 0) {
            read_done_fail_count++;
        } else if (valid_msg) {
            read_done_seq = msg_seq;
        }
        log_datafifo_read_done(msg_seq,
                               read_done_seq,
                               read_done_fail_count,
                               read_done_ret,
                               copy_cost_ms,
                               read_done_cost_ms,
                               pre_done_cost_ms,
                               msg_total_len,
                               msg_flags,
                               !valid_msg ||
                               stale_frame ||
                               late_frame ||
                               (should_copy && copy_ret != 0) ||
                               read_done_ret != 0 ||
                               copy_cost_ms > 5U ||
                               pre_done_cost_ms > 10U ||
                               read_done_cost_ms > 5U ||
                               (msg_flags & MPP_NALU_IPC_FLAG_SNAPSHOT));

        if (valid_msg && !stale_frame && should_copy && copy_ret == 0) {
            if (can_send) {
                send_start_ms = monotonic_ms();
            }

            prev_sent_seq = last_sent_seq;
            if (can_send) {
                frame_rtp_ts = rtp_clock_next_timestamp(&rtp_clock,
                                                        owned_frame.frame.frame_pts,
                                                        &pts_mode,
                                                        &pts_delta_us,
                                                        &rtp_delta);
                timestamp = frame_rtp_ts;
            }
            send_ret = RTP_SEND_OK;
            for (int ti = 0; ti < target_count; ti++) {
                int slot = targets[ti].slot;
                int target_need_parameter_sets;
                int target_ret;
                if (slot < 0 || slot >= RTSP_MAX_CLIENTS)
                    continue;
                target_need_parameter_sets = targets[ti].need_parameter_sets;
                target_ret = datafifo_process_copied_frame_for_playback(
                    sock,
                    &targets[ti].rtp_addr,
                    &owned_frame.frame,
                    &target_states[slot].seq,
                    frame_rtp_ts,
                    ssrc,
                    1,
                    &target_need_parameter_sets,
                    &target_states[slot].wait_for_idr);
                if (target_need_parameter_sets != targets[ti].need_parameter_sets)
                    rtsp_target_mark_parameter_sets_sent(slot);
                if (target_ret != RTP_SEND_OK) {
                    send_ret = target_ret;
                    target_states[slot].wait_for_idr = 1;
                    rtsp_target_request_parameter_sets(slot);
                    g_wait_random_access_skip_count = 0;
                    if (target_ret == RTP_SEND_DROP)
                        rtp_drop_count++;
                } else {
                    target_states[slot].last_sent_seq = owned_frame.frame.seq;
                }
            }
            if (send_ret != RTP_SEND_OK)
                send_failed = 1;

            if (can_send) {
                send_cost_ms = monotonic_ms() - send_start_ms;
                if (!send_failed) {
                    last_sent_seq = owned_frame.frame.seq;
                }
                if (send_failed) {
                    printf("[rtp] send ret=%d frame_seq=%llu cost=%llums drop_count=%llu, wait random access\n",
                           send_ret,
                           (unsigned long long)owned_frame.frame.seq,
                           (unsigned long long)send_cost_ms,
                           (unsigned long long)rtp_drop_count);
                }
            } else {
                printf("[datafifo] seq=%llu no RTP target play=%d dest=%d flags=0x%x\n",
                       (unsigned long long)owned_frame.frame.seq,
                       playing,
                       dest_valid,
                       owned_frame.frame.reserved);
            }

            /*
             * Feed every copied frame to the snapshot parser so it can keep
             * VPS/SPS/PPS and the current decodable GOP cached.  The parser
             * itself only enqueues a file when the snapshot flag is set.
             */
            snapshot_ret = datafifo_snapshot_process_copied(&owned_frame.frame,
                                                            g_snapshot_ready);
            if ((msg_flags & MPP_NALU_IPC_FLAG_SNAPSHOT) != 0) {
                printf("[snapshot] copied seq=%llu process ret=%d flags=0x%x\n",
                       (unsigned long long)owned_frame.frame.seq,
                       snapshot_ret,
                       owned_frame.frame.reserved);
            }

            frame_count++;
            log_datafifo_frame_stats(owned_frame.frame.seq,
                                     owned_frame.frame.pack_cnt,
                                     owned_frame.frame.total_len,
                                     owned_frame.frame.frame_pts,
                                     owned_frame.frame.submit_time_ms,
                                     msg_age_ms,
                                     owned_frame.frame.reserved,
                                     can_send,
                                     prev_sent_seq,
                                     send_cost_ms,
                                     pts_delta_us,
                                     rtp_delta,
                                     frame_rtp_ts,
                                     pts_mode,
                                     frame_count,
                                     need_parameter_sets ||
                                     msg_age_ms > DATAFIFO_LOW_LATENCY_MAX_AGE_MS ||
                                     send_cost_ms > (1000U / VIDEO_FPS) ||
                                     send_failed ||
                                     snapshot_ret != 0);
            datafifo_owned_frame_free(&owned_frame);
        } else if (valid_msg && !stale_frame) {
            frame_count++;
            if (!can_send && (frame_count % DATAFIFO_LOG_INTERVAL) == 0) {
                printf("[datafifo] seq=%llu no copy needed play=%d dest=%d flags=0x%x\n",
                       (unsigned long long)msg_seq,
                       playing,
                       dest_valid,
                       msg_flags);
            }
        }
    }

    nalu_datafifo_close(&reader);
    return NULL;
}

static void *rtp_sender_thread(void *arg)
{
    int sock;

    (void)arg;

    sock = create_udp_socket_only();
    if (sock < 0) {
        printf("[rtp] failed to create RTP socket\n");
        return NULL;
    }

    printf("[rtp] RTP socket bound to UDP port %d\n", RTP_SERVER_PORT);

    if (g_source == STREAM_SOURCE_DATAFIFO) {
        datafifo_rtp_sender_loop(sock);
    } else {
        file_rtp_sender_loop(sock);
    }

    close(sock);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t sender_tid;
    int listen_fd;
// 解析命令行参数
    if (parse_args(argc, argv) != 0) {
        print_usage(argv[0]);
        return -1;
    }
    if (g_source == STREAM_SOURCE_DATAFIFO &&
        ctrl_fifo_open_writer(g_ctrl_fifo_phy_addr) != 0) {
        printf("[ctrl] disabled because writer open failed\n");
    }
// 初始化快照写入线程
    if (g_source == STREAM_SOURCE_DATAFIFO) {
        if (snapshot_writer_init(SNAPSHOT_DEFAULT_DIR) == 0) {
            g_snapshot_ready = 1;
        } else {
            printf("[snapshot] writer init failed, DATAFIFO snapshots disabled\n");
        }
    }
// 创建RTP发送线程
    if (pthread_create(&sender_tid, NULL, rtp_sender_thread, NULL) != 0) {
        printf("pthread_create RTP sender failed\n");
        cleanup_resources();
        return -1;
    }
    pthread_detach(sender_tid);
// 启动RTSP服务器
    listen_fd = start_rtsp_server();
    if (listen_fd < 0) {
        cleanup_resources();
        return -1;
    }
// 处理RTSP客户端连接
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd;

        client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            printf("accept failed, errno=%d\n", errno);
            continue;
        }

        if (rtsp_start_client_thread(client_fd, &cli_addr) != 0) {
            printf("[rtsp] failed to start client thread, errno=%d\n", errno);
            close(client_fd);
        } else {
            printf("[rtsp] accepted client %s:%d\n",
                   inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
        }
    }
// 关闭RTSP服务器监听套接字
    close(listen_fd);
    cleanup_resources();
    return 0;
}
