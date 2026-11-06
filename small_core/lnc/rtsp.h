#ifndef RTSP_H
#define RTSP_H

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h> 
#include <sys/socket.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rtp.h"

#define RTSP_PORT        554
#define RTSP_SESSION_ID  "12345678"

typedef struct
{
    char method[32];
    char url[256];
    int cseq;
} rtsp_request_t;

extern pthread_mutex_t g_mutex;
extern pthread_cond_t g_cond;
#define RTSP_MAX_CLIENTS 8

typedef struct {
    int slot;
    int playing;
    int need_parameter_sets;
    unsigned int generation;
    struct sockaddr_in rtp_addr;
} rtsp_target_t;

/* Kept as aggregate status for existing diagnostics. */
extern int g_play;
extern int g_client_addr_set;
extern int g_need_parameter_sets;
extern unsigned int g_rtsp_session_generation;
extern struct sockaddr_in g_client_addr;

int rtsp_client_setup(const struct sockaddr_in *peer,
                      int client_rtp_port,
                      int *slot_out,
                      unsigned int *generation_out);
void rtsp_client_play(int slot);
void rtsp_client_remove(int slot);
int rtsp_get_targets(rtsp_target_t *targets, int max_targets);
void rtsp_target_mark_parameter_sets_sent(int slot);
void rtsp_target_request_parameter_sets(int slot);
int rtsp_start_client_thread(int client_fd, const struct sockaddr_in *cli_addr);

// 处理RTSP客户端
void handle_rtsp_client(int client_fd, const struct sockaddr_in *cli_addr);

int start_rtsp_server(void);

#endif
