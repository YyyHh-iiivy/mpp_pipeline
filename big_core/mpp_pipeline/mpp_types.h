#ifndef MPP_TYPES_H
#define MPP_TYPES_H

#include "k_type.h"
#include "k_venc_comm.h"

#define MPP_MAX_STREAM_PACKS 8

typedef enum {
    STREAM_EXPORT_LOCAL_LOG = 0,
    STREAM_EXPORT_RESERVED_IPC = 1
} stream_export_mode;

typedef struct {
    k_u64 phys_addr;
    k_u64 virt_addr;
    k_u32 len;
    k_u32 type;
} mpp_stream_pack_desc;

typedef struct {
    k_u32 chn;
    k_u64 pts;
    k_u32 pack_cnt;
    mpp_stream_pack_desc packs[MPP_MAX_STREAM_PACKS];
} mpp_stream_frame_desc;

typedef struct {
    k_u32 event_id;
    k_u64 detect_time_ms;
    k_u32 motion_score;
    k_u32 osd_duration_ms;
    k_u32 request_snapshot;
} motion_event_msg;

typedef struct {
    k_u32 event_id;
    k_u64 frame_time_ms;
    k_u32 source_chn;
    char path_hint[64];
} snapshot_request_msg;

typedef struct {
    k_u64 frame_id;
    k_u64 timestamp_ms;
    k_u32 width;
    k_u32 height;
    k_u32 stride;
    const k_u8 *y;
} ai_gray_frame_view;

typedef struct {
    k_u32 is_motion;
    k_u32 motion_score;
} motion_detect_result;

#endif /* MPP_TYPES_H */
