#include "rtsp.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    int active;
    int playing;
    int need_parameter_sets;
    unsigned int generation;
    struct sockaddr_in rtp_addr;
} rtsp_client_state_t;

static rtsp_client_state_t g_clients[RTSP_MAX_CLIENTS];

static int send_all(int fd, const char *data)
{
    size_t total = strlen(data);
    size_t sent = 0;

    while (sent < total)
    {
        int n = send(fd, data + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}
// 解析RTSP请求
static int parse_rtsp_request(const char *req, rtsp_request_t *out)
{
    const char *cseq_pos;

    memset(out, 0, sizeof(*out));
    out->cseq = 0;

    if (sscanf(req, "%31s %255s", out->method, out->url) != 2)
    {
        return -1;
    }

    cseq_pos = strstr(req, "CSeq:");
    if (cseq_pos == NULL)
    {
        cseq_pos = strstr(req, "Cseq:");
    }
    if (cseq_pos != NULL)
    {
        out->cseq = atoi(cseq_pos + 5);
    }

    return 0;
}
// 解析客户端端口
static int parse_client_port(const char *req)
{
    const char *p = strstr(req, "client_port=");
    int port;

    if (p == NULL)
    {
        return 0;
    }

    port = atoi(p + strlen("client_port="));
    if (port <= 0 || port > 65535)
    {
        return 0;
    }

    return port;
}
// 生成SDP描述
static void make_sdp(char *sdp, size_t maxlen, const char *control_url)
{
    snprintf(sdp,
             maxlen,
             "v=0\r\n"
             "o=- 0 0 IN IP4 0.0.0.0\r\n"
             "s=H265 Stream\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H265/90000\r\n"
             "a=framerate:%d\r\n"
             "a=control:%s/trackID=0\r\n",
             RTP_PAYLOAD_TYPE,
             RTP_PAYLOAD_TYPE,
             VIDEO_FPS,
             control_url);
}
// 发送OPTIONS响应
static void send_options_response(int fd, int cseq)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
             "\r\n",
             cseq);

    send_all(fd, response);
}
// 发送DESCRIBE响应
static void send_describe_response(int fd, const rtsp_request_t *req)
{
    char sdp[1024];
    char response[2048];

    make_sdp(sdp, sizeof(sdp), req->url);

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %lu\r\n"
             "\r\n"
             "%s",
             req->cseq,
             (unsigned long)strlen(sdp),
             sdp);

    send_all(fd, response);
}

static void send_setup_response(int fd, int cseq, int client_rtp_port)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Transport: RTP/AVP/UDP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
             "Session: %s\r\n"
             "\r\n",
             cseq,
             client_rtp_port,
             client_rtp_port + 1,
             RTP_SERVER_PORT,
             RTCP_SERVER_PORT,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_play_response(int fd, int cseq)
{
    char response[512];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %s\r\n"
             "Range: npt=0.000-\r\n"
             "\r\n",
             cseq,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_teardown_response(int fd, int cseq)
{
    char response[256];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %s\r\n"
             "\r\n",
             cseq,
             RTSP_SESSION_ID);

    send_all(fd, response);
}

static void send_error_response(int fd, int cseq, int code, const char *reason)
{
    char response[256];

    snprintf(response,
             sizeof(response),
             "RTSP/1.0 %d %s\r\n"
             "CSeq: %d\r\n"
             "\r\n",
             code,
             reason,
             cseq);

    send_all(fd, response);
}

static void update_aggregate_state_locked(void)
{
    int i;
    g_play = 0;
    g_client_addr_set = 0;
    g_need_parameter_sets = 0;
    memset(&g_client_addr, 0, sizeof(g_client_addr));
    for (i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (!g_clients[i].active)
            continue;
        g_client_addr_set = 1;
        if (g_clients[i].playing) {
            g_play = 1;
            g_client_addr = g_clients[i].rtp_addr;
        }
        if (g_clients[i].need_parameter_sets)
            g_need_parameter_sets = 1;
    }
}

int rtsp_client_setup(const struct sockaddr_in *peer,
                      int client_rtp_port,
                      int *slot_out,
                      unsigned int *generation_out)
{
    int i;
    if (peer == NULL || client_rtp_port <= 0 || client_rtp_port > 65535)
        return -1;
    pthread_mutex_lock(&g_mutex);
    for (i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (!g_clients[i].active)
            break;
    }
    if (i == RTSP_MAX_CLIENTS) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    memset(&g_clients[i], 0, sizeof(g_clients[i]));
    g_clients[i].active = 1;
    g_clients[i].need_parameter_sets = 1;
    g_clients[i].generation = ++g_rtsp_session_generation;
    g_clients[i].rtp_addr = *peer;
    g_clients[i].rtp_addr.sin_port = htons((uint16_t)client_rtp_port);
    update_aggregate_state_locked();
    if (slot_out) *slot_out = i;
    if (generation_out) *generation_out = g_clients[i].generation;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void rtsp_client_play(int slot)
{
    pthread_mutex_lock(&g_mutex);
    if (slot >= 0 && slot < RTSP_MAX_CLIENTS && g_clients[slot].active)
        g_clients[slot].playing = 1;
    update_aggregate_state_locked();
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}

void rtsp_client_remove(int slot)
{
    pthread_mutex_lock(&g_mutex);
    if (slot >= 0 && slot < RTSP_MAX_CLIENTS)
        memset(&g_clients[slot], 0, sizeof(g_clients[slot]));
    update_aggregate_state_locked();
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}

int rtsp_get_targets(rtsp_target_t *targets, int max_targets)
{
    int i;
    int count = 0;
    if (targets == NULL || max_targets <= 0)
        return 0;
    pthread_mutex_lock(&g_mutex);
    for (i = 0; i < RTSP_MAX_CLIENTS && count < max_targets; i++) {
        if (!g_clients[i].active || !g_clients[i].playing)
            continue;
        targets[count].slot = i;
        targets[count].playing = g_clients[i].playing;
        targets[count].need_parameter_sets = g_clients[i].need_parameter_sets;
        targets[count].generation = g_clients[i].generation;
        targets[count].rtp_addr = g_clients[i].rtp_addr;
        count++;
    }
    pthread_mutex_unlock(&g_mutex);
    return count;
}

void rtsp_target_mark_parameter_sets_sent(int slot)
{
    pthread_mutex_lock(&g_mutex);
    if (slot >= 0 && slot < RTSP_MAX_CLIENTS && g_clients[slot].active)
        g_clients[slot].need_parameter_sets = 0;
    update_aggregate_state_locked();
    pthread_mutex_unlock(&g_mutex);
}

void rtsp_target_request_parameter_sets(int slot)
{
    pthread_mutex_lock(&g_mutex);
    if (slot >= 0 && slot < RTSP_MAX_CLIENTS && g_clients[slot].active)
        g_clients[slot].need_parameter_sets = 1;
    update_aggregate_state_locked();
    pthread_mutex_unlock(&g_mutex);
}

// 处理RTSP客户端请求
void handle_rtsp_client(int client_fd, const struct sockaddr_in *cli_addr)
{
    char req_buf[2048];
    int client_slot = -1;

    while (1)
    {
        int n;
        rtsp_request_t req;
// 接收客户端请求
        n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
        if (n <= 0)
        {
            break;
        }

        req_buf[n] = '\0';
        printf("[rtsp] request from %s:%d\n%s\n",
               inet_ntoa(cli_addr->sin_addr),
               ntohs(cli_addr->sin_port),
               req_buf);

        if (parse_rtsp_request(req_buf, &req) != 0)
        {
            send_error_response(client_fd, 0, 400, "Bad Request");
            continue;
        }
// 处理OPTIONS请求
        if (strcmp(req.method, "OPTIONS") == 0)
        {
            send_options_response(client_fd, req.cseq);
        }
// 处理DESCRIBE请求
        else if (strcmp(req.method, "DESCRIBE") == 0)
        {
            send_describe_response(client_fd, &req);
        }
// 处理SETUP请求
        else if (strcmp(req.method, "SETUP") == 0)
        {
            int client_port = parse_client_port(req_buf);
            if (client_port == 0)
            {
                send_error_response(client_fd, req.cseq, 461, "Unsupported Transport");
                continue;
            }
// 互斥量保护，确保线程安全访问共享资源
            if (client_slot >= 0)
                rtsp_client_remove(client_slot);
            if (rtsp_client_setup(cli_addr, client_port, &client_slot, NULL) != 0) {
                send_error_response(client_fd, req.cseq, 453, "Not Enough Bandwidth");
                continue;
            }

            printf("[rtsp] SETUP client slot=%d RTP target %s:%d\n",
                   client_slot,
                   inet_ntoa(cli_addr->sin_addr),
                   client_port);

            send_setup_response(client_fd, req.cseq, client_port);
        }
        else if (strcmp(req.method, "PLAY") == 0)
        {
            if (client_slot < 0) {
                send_error_response(client_fd, req.cseq, 455, "Method Not Valid in This State");
                continue;
            }
            rtsp_client_play(client_slot);

            send_play_response(client_fd, req.cseq);
        }
        else if (strcmp(req.method, "TEARDOWN") == 0)
        {
            rtsp_client_remove(client_slot);
            client_slot = -1;

            send_teardown_response(client_fd, req.cseq);
            break;
        }
        else
        {
            send_error_response(client_fd, req.cseq, 405, "Method Not Allowed");
        }
    }
// 互斥量保护，确保线程安全访问共享资源
    rtsp_client_remove(client_slot);
}

typedef struct {
    int fd;
    struct sockaddr_in addr;
} rtsp_client_thread_arg_t;

static void *rtsp_client_thread(void *arg)
{
    rtsp_client_thread_arg_t *ctx = (rtsp_client_thread_arg_t *)arg;
    int fd = ctx->fd;
    struct sockaddr_in addr = ctx->addr;
    free(ctx);
    handle_rtsp_client(fd, &addr);
    close(fd);
    printf("[rtsp] client disconnected %s:%d\n",
           inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return NULL;
}

int rtsp_start_client_thread(int client_fd, const struct sockaddr_in *cli_addr)
{
    pthread_t tid;
    rtsp_client_thread_arg_t *ctx;
    if (cli_addr == NULL)
        return -1;
    ctx = (rtsp_client_thread_arg_t *)malloc(sizeof(*ctx));
    if (ctx == NULL)
        return -1;
    ctx->fd = client_fd;
    ctx->addr = *cli_addr;
    if (pthread_create(&tid, NULL, rtsp_client_thread, ctx) != 0) {
        free(ctx);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

// 启动RTSP服务器
int start_rtsp_server(void)
{
    int listen_fd;
    int opt = 1;
    struct sockaddr_in listen_addr;
//  创建监听套接字
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("RTSP socket failed, errno=%d\n", errno);
        return -1;
    }
//  设置套接字选项，允许地址重用
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(RTSP_PORT);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
//  绑定监听套接字到指定端口
    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        printf("RTSP bind port %d failed, errno=%d\n", RTSP_PORT, errno);
        close(listen_fd);
        return -1;
    }
//  监听监听套接字
    if (listen(listen_fd, RTSP_MAX_CLIENTS) < 0) {
        printf("RTSP listen failed, errno=%d\n", errno);
        close(listen_fd);
        return -1;
    }

    printf("RTSP server started on port %d\n", RTSP_PORT);
    printf("Open VLC URL: rtsp://<board-ip>/stream\n");
    return listen_fd;
}
