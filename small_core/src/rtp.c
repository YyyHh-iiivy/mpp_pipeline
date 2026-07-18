#include "rtp.h"

#include <fcntl.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <time.h>

#define RTP_BUSY_LOG_INTERVAL_MS 1000ULL

#if RTP_STARTUP_DUPLICATE_PACKETS
static int g_rtp_duplicate_packets;
#endif

static uint64_t rtp_log_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)(ts.tv_nsec / 1000000L);
}

void rtp_set_packet_duplicate(int duplicate)
{
#if RTP_STARTUP_DUPLICATE_PACKETS
    g_rtp_duplicate_packets = duplicate ? 1 : 0;
    printf("[rtp] packet duplicate %s\n", g_rtp_duplicate_packets ? "on" : "off");
#else
    (void)duplicate;
#endif
}

static int rtp_errno_is_temporary(int err)
{
    if (err == EAGAIN || err == EINTR) {
        return 1;
    }
#ifdef EWOULDBLOCK
    if (err == EWOULDBLOCK) {
        return 1;
    }
#endif
#ifdef ENOBUFS
    if (err == ENOBUFS) {
        return 1;
    }
#endif
    return 0;
}

int rtp_get_send_queue_bytes(int sock, uint32_t *bytes)
{
    int queued_bytes = 0;

    if (sock < 0 || bytes == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ioctl(sock, SIOCOUTQ, &queued_bytes) != 0 || queued_bytes < 0) {
        return -1;
    }

    *bytes = (uint32_t)queued_bytes;
    return 0;
}

int rtp_get_actual_send_buffer(int sock, uint32_t *bytes)
{
    int actual_bytes = 0;
    socklen_t opt_len = sizeof(actual_bytes);

    if (sock < 0 || bytes == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &actual_bytes, &opt_len) != 0 ||
        actual_bytes < 0) {
        return -1;
    }

    *bytes = (uint32_t)actual_bytes;
    return 0;
}

static void configure_rtp_socket_low_latency(int sock)
{
    int flags;
    int sndbuf = RTP_SOCKET_SNDBUF;
    uint32_t actual_sndbuf = 0;
#ifdef IP_TOS
    int tos = RTP_IP_TOS_LOWDELAY;
#endif

    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0) {
        printf("[rtp] set SO_SNDBUF=%d failed, errno=%d\n", sndbuf, errno);
    }

#ifdef IP_TOS
    if (setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) != 0) {
        printf("[rtp] set IP_TOS lowdelay failed, errno=%d\n", errno);
    }
#endif

    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        printf("[rtp] fcntl F_GETFL failed, errno=%d\n", errno);
        return;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0) {
        printf("[rtp] fcntl O_NONBLOCK failed, errno=%d\n", errno);
        return;
    }

    printf("[rtp] UDP socket low-latency mode: nonblock=1 sndbuf=%d\n", sndbuf);
    if (rtp_get_actual_send_buffer(sock, &actual_sndbuf) == 0) {
        printf("[rtp] UDP socket buffers: sndbuf_request=%d sndbuf_actual=%u\n",
               sndbuf,
               actual_sndbuf);
    } else {
        printf("[rtp] get actual SO_SNDBUF failed, errno=%d\n", errno);
    }
}


int create_udp_socket_only(void)
{
    int sock;
    int opt = 1;
    struct sockaddr_in local_addr;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        printf("udp socket() failed, errno=%d\n", errno);
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    configure_rtp_socket_low_latency(sock);

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(RTP_SERVER_PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        printf("udp bind port %d failed, errno=%d\n", RTP_SERVER_PORT, errno);
        close(sock);
        return -1;
    }

    return sock;
}



//构建RTP头
static void build_rtp_header(uint8_t *packet,
                             uint16_t seq,
                             uint32_t timestamp,
                             uint32_t ssrc,
                             uint8_t marker)
{
    packet[0] = 0x80;//RTP头起始码，必须是0x80
    // 标记位，0表示不是最后一个包，1表示是最后一个包
    packet[1] = (uint8_t)((marker ? 0x80 : 0x00) | (RTP_PAYLOAD_TYPE & 0x7f));
// 序号，16位
    packet[2] = (uint8_t)((seq >> 8) & 0xff);
    packet[3] = (uint8_t)(seq & 0xff);
// 时间戳，32位
    packet[4] = (uint8_t)((timestamp >> 24) & 0xff);
    packet[5] = (uint8_t)((timestamp >> 16) & 0xff);
    packet[6] = (uint8_t)((timestamp >> 8) & 0xff);
    packet[7] = (uint8_t)(timestamp & 0xff);
// SSSRC，32位
    packet[8] = (uint8_t)((ssrc >> 24) & 0xff);
    packet[9] = (uint8_t)((ssrc >> 16) & 0xff);
    packet[10] = (uint8_t)((ssrc >> 8) & 0xff);
    packet[11] = (uint8_t)(ssrc & 0xff);
}

//通过查找起始码找到RTP包的位置和长度
static long find_start_code(const uint8_t *buf,
                            size_t size,
                            size_t offset,
                            size_t *start_code_len)
{
    size_t i;
// 查找RTP包的起始码
    for (i = offset; i + 3 <= size; i++)
    {
        if (i + 3 <= size &&
            buf[i] == 0x00 &&
            buf[i + 1] == 0x00 &&
            buf[i + 2] == 0x01)
        {
            *start_code_len = 3;
            return (long)i;
        }
// 查找RTP包的起始码，4字节
        if (i + 4 <= size &&
            buf[i] == 0x00 &&
            buf[i + 1] == 0x00 &&
            buf[i + 2] == 0x00 &&
            buf[i + 3] == 0x01)
        {
            *start_code_len = 4;
            return (long)i;
        }
    }

    return -1;
}

static int buffer_starts_with_start_code(const uint8_t *buf,
                                         size_t len,
                                         size_t *start_code_len)
{
    if (buf == NULL || len < 3 || start_code_len == NULL)
    {
        return 0;
    }

    if (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01)
    {
        *start_code_len = 3;
        return 1;
    }

    if (len >= 4 &&
        buf[0] == 0x00 &&
        buf[1] == 0x00 &&
        buf[2] == 0x00 &&
        buf[3] == 0x01)
    {
        *start_code_len = 4;
        return 1;
    }

    return 0;
}

//把NALU从文件中读取出来并保存到数组中
int load_annexb_nalus(const uint8_t *buf,
                             size_t size,
                             nalu_t **out_nalus,
                             size_t *out_count)
{
    nalu_t *nalus = NULL;
    size_t capacity = 0;
    size_t count = 0;
    size_t search_offset = 0;

    while (1)
    {
        size_t sc_len = 0;
        size_t next_sc_len = 0;
        long sc_pos;
        long next_sc_pos;
        size_t nalu_start;
        size_t nalu_end;

        sc_pos = find_start_code(buf, size, search_offset, &sc_len);
        if (sc_pos < 0)
        {
            break;
        }

        nalu_start = (size_t)sc_pos + sc_len;
        next_sc_pos = find_start_code(buf, size, nalu_start, &next_sc_len);
        nalu_end = (next_sc_pos < 0) ? size : (size_t)next_sc_pos;

        while (nalu_end > nalu_start && buf[nalu_end - 1] == 0x00)
        {
            nalu_end--;
        }

        if (nalu_end > nalu_start)
        {
            if (count == capacity)
            {
                size_t new_capacity = (capacity == 0) ? 128 : capacity * 2;
                nalu_t *new_nalus = (nalu_t *)realloc(nalus, new_capacity * sizeof(nalu_t));
                if (new_nalus == NULL)
                {
                    free(nalus);
                    return -1;
                }
                nalus = new_nalus;
                capacity = new_capacity;
            }

            nalus[count].data = buf + nalu_start;
            nalus[count].len = nalu_end - nalu_start;
            count++;
        }

        if (next_sc_pos < 0)
        {
            break;
        }

        search_offset = (size_t)next_sc_pos;
    }

    *out_nalus = nalus;
    *out_count = count;
    return 0;
}

//获取NALU类型
int h265_nalu_type(const uint8_t *nalu, size_t len)
{
    if (len < 2)
    {
        return -1;
    }

    return (nalu[0] >> 1) & 0x3f;
}

//发送RTP包（未分片）
static int send_rtp_packet(int sock,
                           const struct sockaddr_in *dest,
                           const uint8_t *payload,
                           size_t payload_len,
                           uint16_t *seq,
                           uint32_t timestamp,
                           uint32_t ssrc,
                           uint8_t marker)
{
    uint8_t packet[RTP_PACKET_MAX_SIZE];
    size_t packet_len;
    ssize_t sent_len;
    int err;
    static unsigned int drop_log_count;
    static uint64_t last_busy_log_ms;

    packet_len = RTP_HEADER_SIZE + payload_len;
    if (packet_len > sizeof(packet))
    {
        printf("RTP packet too large: %lu\n", (unsigned long)packet_len);
        return RTP_SEND_FATAL;
    }

    build_rtp_header(packet, *seq, timestamp, ssrc, marker);
    memcpy(packet + RTP_HEADER_SIZE, payload, payload_len);

    sent_len = sendto(sock,
                      packet,
                      packet_len,
                      0,
                      (const struct sockaddr *)dest,
                      sizeof(*dest));
    if (sent_len < 0)
    {
        err = errno;
        if (rtp_errno_is_temporary(err)) {
            uint64_t now_ms = rtp_log_now_ms();

            drop_log_count++;
            if (drop_log_count == 1U ||
                (now_ms != 0 &&
                 (last_busy_log_ms == 0 ||
                  now_ms - last_busy_log_ms >= RTP_BUSY_LOG_INTERVAL_MS)) ||
                (now_ms == 0 && (drop_log_count % 30U) == 0U)) {
                printf("[rtp] send queue busy, drop packet seq=%u errno=%d drops=%u\n",
                       (unsigned int)*seq,
                       err,
                       drop_log_count);
                last_busy_log_ms = now_ms;
            }
            return RTP_SEND_DROP;
        }

        printf("sendto failed, seq=%u errno=%d\n",
               (unsigned int)*seq,
               err);
        return RTP_SEND_FATAL;
    }

    if ((size_t)sent_len != packet_len)
    {
        printf("[rtp] sendto short packet seq=%u sent=%ld expect=%lu\n",
               (unsigned int)*seq,
               (long)sent_len,
               (unsigned long)packet_len);
        return RTP_SEND_DROP;
    }

#if RTP_STARTUP_DUPLICATE_PACKETS
    if (g_rtp_duplicate_packets)
    {
        ssize_t dup_len;

        dup_len = sendto(sock,
                         packet,
                         packet_len,
                         0,
                         (const struct sockaddr *)dest,
                         sizeof(*dest));
        if (dup_len < 0 && !rtp_errno_is_temporary(errno))
        {
            printf("[rtp] duplicate send failed, seq=%u errno=%d\n",
                   (unsigned int)*seq,
                   errno);
        }
    }
#endif

    (*seq)++;
#if RTP_PACKET_PACE_US > 0
    usleep(RTP_PACKET_PACE_US);
#endif
    return RTP_SEND_OK;
}

//
int send_h265_nalu_rtp(int sock,
                              const struct sockaddr_in *dest,
                              const uint8_t *nalu,
                              size_t nalu_len,
                              uint16_t *seq,
                              uint32_t timestamp,
                              uint32_t ssrc,
                              uint8_t marker)
{
    int nal_type;

    if (nalu == NULL || nalu_len < 2)
    {
        return RTP_SEND_FATAL;
    }

    nal_type = h265_nalu_type(nalu, nalu_len);

    if (nalu_len <= RTP_MAX_PAYLOAD)//不用分片，直接发送
    {
        return send_rtp_packet(sock, dest, nalu, nalu_len, seq, timestamp, ssrc, marker);
    }
    //需要分片
    {
        uint8_t fu_payload[RTP_MAX_PAYLOAD];
        size_t max_fragment_data = RTP_MAX_PAYLOAD - H265_FU_HEADER_SIZE;//分片数据最大容量
        size_t offset = 2;//头部两字节为头部
        int first = 1;
        int send_ret;

        while (offset < nalu_len)
        {
            size_t remaining = nalu_len - offset;
            size_t fragment_size = remaining > max_fragment_data ? max_fragment_data : remaining;
            int last = (offset + fragment_size) >= nalu_len;//判断是否为最后一次

            //设置NALU为分片类型
            fu_payload[0] = (uint8_t)((nalu[0] & 0x81) | (H265_FU_TYPE << 1));
            fu_payload[1] = nalu[1];
            fu_payload[2] = (uint8_t)((first ? 0x80 : 0x00) |
                                      (last ? 0x40 : 0x00) |
                                      (nal_type & 0x3f));
            
            memcpy(fu_payload + H265_FU_HEADER_SIZE, nalu + offset, fragment_size);

            send_ret = send_rtp_packet(sock,
                                       dest,
                                       fu_payload,
                                       H265_FU_HEADER_SIZE + fragment_size,
                                       seq,
                                       timestamp,
                                       ssrc,
                                       last ? marker : 0);
            if (send_ret != RTP_SEND_OK)
            {
                return send_ret;
            }

            first = 0;
            offset += fragment_size;
        }
    }

    return RTP_SEND_OK;
}
// 发送H265缓冲区数据（未分片）
int send_h265_buffer_rtp(int sock,
                         const struct sockaddr_in *dest,
                         const uint8_t *buf,
                         size_t len,
                         uint16_t *seq,
                         uint32_t timestamp,
                         uint32_t ssrc,
                         uint8_t last_marker)
{
    size_t first_sc_len = 0;

    if (buf == NULL || len < 2)
    {
        return RTP_SEND_FATAL;
    }

    /*
     * VENC pack may be one raw NALU, one Annex-B NALU, or several Annex-B
     * NALUs in the same pack. RTP/H.265 payload must not include Annex-B
     * start codes, so split and strip them when present.
     */
    if (!buffer_starts_with_start_code(buf, len, &first_sc_len))
    {
        return send_h265_nalu_rtp(sock, dest, buf, len, seq, timestamp, ssrc, last_marker);
    }

    {
        size_t search_offset = 0;
        int sent_count = 0;

        while (1)
        {
            size_t sc_len = 0;
            size_t next_sc_len = 0;
            long sc_pos;
            long next_sc_pos;
            size_t nalu_start;
            size_t nalu_end;
            uint8_t packet_marker;

            sc_pos = find_start_code(buf, len, search_offset, &sc_len);
            if (sc_pos < 0)
            {
                break;
            }

            nalu_start = (size_t)sc_pos + sc_len;
            next_sc_pos = find_start_code(buf, len, nalu_start, &next_sc_len);
            nalu_end = (next_sc_pos < 0) ? len : (size_t)next_sc_pos;

            while (nalu_end > nalu_start && buf[nalu_end - 1] == 0x00)
            {
                nalu_end--;
            }

            if (nalu_end > nalu_start)
            {
                int send_ret;
                packet_marker = (next_sc_pos < 0) ? last_marker : 0;
                send_ret = send_h265_nalu_rtp(sock,
                                              dest,
                                              buf + nalu_start,
                                              nalu_end - nalu_start,
                                              seq,
                                              timestamp,
                                              ssrc,
                                              packet_marker);
                if (send_ret != RTP_SEND_OK)
                {
                    return send_ret;
                }
                sent_count++;
            }

            if (next_sc_pos < 0)
            {
                break;
            }

            search_offset = (size_t)next_sc_pos;
        }

        return (sent_count > 0) ? RTP_SEND_OK : RTP_SEND_FATAL;
    }
}

// 发送DATAFIFO包
int send_datafifo_pack(int sock,
                       const struct sockaddr_in *dest,
                       uint64_t datafifo_seq,
                       k_u32 pack_index,
                       const mpp_nalu_ipc_pack *pack,
                       uint16_t *seq,
                       uint32_t timestamp,
                       uint32_t ssrc,
                       uint8_t marker)
{
    void *virt_addr;
    int ret;
    int munmap_ret;
// 映射DATAFIFO包到虚拟地址
    virt_addr = nalu_datafifo_mmap_pack(pack);
    if (virt_addr == NULL) {
        printf("[datafifo] seq=%llu pack[%u] mmap failed phys=0x%llx len=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               pack ? (unsigned long long)pack->phys_addr : 0ULL,
               pack ? pack->len : 0U);
        return RTP_SEND_FATAL;
    }
// 打印映射信息
    if (NALU_DATAFIFO_VERBOSE_LOG) {
        printf("[datafifo] seq=%llu pack[%u] mmap ok virt=%p phys=0x%llx len=%u marker=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               virt_addr,
               (unsigned long long)pack->phys_addr,
               pack->len,
               (unsigned int)marker);
    }
// 发送H265缓冲区数据（未分片）
    ret = send_h265_buffer_rtp(sock,
                               dest,
                               (const uint8_t *)virt_addr,
                               pack->len,
                               seq,
                               timestamp,
                               ssrc,
                               marker);

    if (NALU_DATAFIFO_VERBOSE_LOG || ret != 0) {
        printf("[datafifo] seq=%llu pack[%u] rtp ret=%d next_rtp_seq=%u\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               ret,
               seq ? (unsigned int)*seq : 0U);
    }

    munmap_ret = nalu_datafifo_munmap_pack(pack, virt_addr);
    if (NALU_DATAFIFO_VERBOSE_LOG || munmap_ret != 0) {
        printf("[datafifo] seq=%llu pack[%u] munmap ret=%d\n",
               (unsigned long long)datafifo_seq,
               (unsigned int)pack_index,
               munmap_ret);
    }
    if (munmap_ret != 0 && ret == RTP_SEND_OK) {
        ret = RTP_SEND_FATAL;
    }

    return ret;
}
