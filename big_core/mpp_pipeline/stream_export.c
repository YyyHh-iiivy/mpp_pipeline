#include "mpp_pipeline.h"

static stream_export_mode g_stream_export_mode = STREAM_EXPORT_LOCAL_LOG;
static k_u32 g_stream_export_frame_count = 0;
static k_bool g_stream_export_inited = K_FALSE;

k_s32 stream_export_init(stream_export_mode mode)
{
    g_stream_export_mode = mode;
    g_stream_export_frame_count = 0;
    g_stream_export_inited = K_TRUE;

    LOG("Stream export init: mode=%d", mode);
    return 0;
}

k_s32 stream_export_submit(const mpp_stream_frame_desc *frame)
{
    if (!frame)
        return -1;

    if (g_stream_export_mode != STREAM_EXPORT_LOCAL_LOG)
        return 0;

    for (k_u32 i = 0; i < frame->pack_cnt; i++) {
        if (frame->packs[i].type != K_VENC_HEADER) {
            g_stream_export_frame_count++;
            LOG("Get NALU, Size: %u bytes  [frame #%u]",
                frame->packs[i].len, g_stream_export_frame_count);
        } else {
            LOG("Get NALU (SPS/PPS header), Size: %u bytes", frame->packs[i].len);
        }
    }

    return 0;
}

void stream_export_deinit(void)
{
    if (!g_stream_export_inited)
        return;

    LOG("Stream export deinit");
    g_stream_export_inited = K_FALSE;
}
