#include "player_codec.h"
#include "player_codec_hooks.h"
#include "player_felix.h"
#include "avcodec.h"
#include "h264dec.h"
#include <x2600_hal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_MAX (512u * 1024u)

struct player_codec_slot {
    AVFrame frame;
    AVBuffer planes[2];
    player_video_token_t token;
    uint64_t frame_index;
    int64_t pts_us, duration_us;
};

struct player_codec {
    AVCodecContext avctx;
    H264Context h264;
    player_video_pool_t *pool;
    player_video_layout_t layout;
    struct player_codec_slot slots[PLAYER_VIDEO_SLOTS];
    uint32_t session;
    uint64_t submitting_frame;
    uint64_t output_count;
    int64_t submitting_pts_us, submitting_duration_us;
    int running;
};

static player_codec_t *active_codec;

static int valid_session(player_codec_t *codec, uint32_t session)
{
    return codec && codec->running && codec->session == session && codec->pool->session == session;
}

int player_codec_format_from_hal(uint32_t hal_format)
{
    return hal_format == 8 ? VPU_FORMAT_NV12 : -EINVAL;
}

AVFrame *player_codec_frame_acquire(H264Context *h)
{
    player_codec_t *codec = h->player_owner;
    player_video_token_t token;
    struct player_codec_slot *slot;
    const SPS *sps = h->ps.sps;
    if (!codec || !codec->running || !sps || h->width != (int)codec->layout.width ||
        h->height != (int)codec->layout.height ||
        (sps->profile_idc != 66 && sps->profile_idc != 77 && sps->profile_idc != 100) ||
        sps->level_idc > 40 || sps->ref_frame_count > 6 || sps->num_reorder_frames > 0 ||
        sps->chroma_format_idc != 1 || sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8 ||
        !sps->frame_mbs_only_flag || sps->mb_aff || !h->ps.pps ||
        h->ps.pps->slice_group_count != 1 || !player_video_codec_acquire(codec->pool, &token))
        return NULL;
    slot = &codec->slots[token.slot];
    slot->token = token;
    slot->frame_index = codec->submitting_frame;
    slot->pts_us = slot->frame.pts = codec->submitting_pts_us;
    slot->duration_us = codec->submitting_duration_us;
    slot->frame.data[0] = slot->planes[0].buffer;
    slot->frame.data[1] = slot->planes[1].buffer;
    slot->frame.linesize[0] = slot->frame.linesize[1] = (int)codec->layout.stride;
    return &slot->frame;
}

void player_codec_frame_release(H264Context *h, AVFrame *frame)
{
    player_codec_t *codec = h->player_owner;
    struct player_codec_slot *slot;
    if (!codec || !frame) return;
    slot = frame->opaque;
    (void)player_video_codec_release(codec->pool, slot->token);
}

int player_codec_frame_publish(H264Context *h, AVFrame *frame)
{
    player_codec_t *codec = h->player_owner;
    struct player_codec_slot *slot = frame->opaque;
    if (!codec || !player_video_codec_publish(codec->pool, slot->token)) return -EINVAL;
    /* CPU never writes NV12 planes after this point; DPU reads physical memory. */
    __asm__ volatile("sync" ::: "memory");
    return 0;
}

int player_codec_open(player_codec_t **result, player_video_pool_t *pool,
                      uint32_t session, const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS])
{
    return player_codec_open_geometry(result, pool, session, buffers, 720, 1280);
}

int player_codec_open_geometry(player_codec_t **result, player_video_pool_t *pool,
                               uint32_t session, const player_codec_buffer_t buffers[PLAYER_VIDEO_SLOTS],
                               uint32_t width, uint32_t height)
{
    player_codec_t *codec;
    uint32_t i;
    int error;
    if (!result || !pool || !buffers || !session || pool->session != session) return -EINVAL;
    if (!((width == 720 && height == 1280) || (width == 1280 && height == 720))) return -ENOTSUP;
    *result = NULL;
    if (active_codec) return -EBUSY;
    codec = calloc(1, sizeof(*codec));
    if (!codec) return -ENOMEM;
    codec->pool = pool; codec->session = session;
    error = player_video_layout_nv12(width, height, &codec->layout);
    if (error) { free(codec); return error; }
    for (i = 0; i < PLAYER_VIDEO_SLOTS; ++i) {
        struct player_codec_slot *slot = &codec->slots[i];
        uint32_t offset = codec->layout.uv_offset;
        if (!player_video_buffer_valid(&codec->layout, (uintptr_t)buffers[i].base, buffers[i].capacity) ||
            ((uintptr_t)buffers[i].base & 0xe0000000u) != 0x80000000u || pool->slots[i].holds) {
            free(codec); return -EINVAL;
        }
        slot->planes[0].buffer = buffers[i].base;
        slot->planes[0].buffer_pa = CPHYSADDR(buffers[i].base);
        slot->planes[0].size = codec->layout.y_bytes;
        slot->planes[1].buffer = buffers[i].base + offset;
        slot->planes[1].buffer_pa = CPHYSADDR(buffers[i].base + offset);
        slot->planes[1].size = codec->layout.uv_bytes;
        slot->frame.buf[0] = &slot->planes[0]; slot->frame.buf[1] = &slot->planes[1];
        slot->frame.width = (int)width; slot->frame.height = (int)height;
        slot->frame.format = VPU_FORMAT_NV12; slot->frame.index = (int)i;
        slot->frame.opaque = slot;
        /* Prevent dirty cache lines from later overwriting VPU output. */
        CleanDCache_by_Addr((unsigned long *)buffers[i].base, codec->layout.total_bytes);
    }
    codec->avctx.priv_data = &codec->h264;
    codec->avctx.codec_id = AV_CODEC_ID_H264;
    codec->avctx.width = (int)width; codec->avctx.height = (int)height;
    codec->h264.player_owner = codec;
    error = h264_decode_init(&codec->avctx);
    if (error < 0) {
        h264_decode_end(&codec->avctx);
        free(codec); return error;
    }
    h264_set_output_format(&codec->h264, player_codec_format_from_hal(8), width, height);
    codec->running = 1; active_codec = codec;
    *result = codec;
    return 0;
}

int player_codec_submit(player_codec_t *codec, uint32_t session,
                        uint8_t *access_unit, uint32_t length, uint32_t capacity,
                        uint64_t frame_index, int64_t pts_us, int64_t duration_us)
{
    AVPacket packet;
    uint32_t i;
    int got = 0, free_slot = 0, result;
    if (!valid_session(codec, session)) return -ESTALE;
    if (!access_unit || !length || length > INPUT_MAX || capacity < length ||
        capacity - length < AV_INPUT_BUFFER_PADDING_SIZE) return -EINVAL;
    /* Match release_unused_pictures before deciding whether a retry is needed. */
    for (i = 0; i < H264_MAX_PICTURE_COUNT; ++i)
        if (codec->h264.DPB[i].f && !codec->h264.DPB[i].reference)
            ff_h264_release_picture(&codec->h264, &codec->h264.DPB[i]);
    for (i = 0; i < PLAYER_VIDEO_SLOTS; ++i)
        if (!codec->pool->slots[i].holds) free_slot = 1;
    if (!free_slot) return -EAGAIN;
    memset(access_unit + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    CleanDCache_by_Addr((unsigned long *)access_unit, length + AV_INPUT_BUFFER_PADDING_SIZE);
    packet.data = access_unit; packet.data_pa = CPHYSADDR(access_unit); packet.size = (int)length;
    codec->submitting_frame = frame_index;
    codec->submitting_pts_us = pts_us;
    codec->submitting_duration_us = duration_us;
    result = h264_decode_frame(&codec->avctx, NULL, &got, &packet);
    if (result < 0) {
        const SPS *sps = codec->h264.ps.sps;
        const jzm_h264 *hardware = codec->h264.st_h264;
        printf("CODEC_ERROR index=%llu pts_us=%lld result=%d profile=%d level=%d refs=%d cabac=%u qp=%u mb=%ux%u bs_addr=%08lx bs_index=%lu bs_bits=%lu rbsp=%lu sch=%08lx sde=%08lx\n",
               (unsigned long long)frame_index, (long long)pts_us, result,
               sps ? sps->profile_idc : -1, sps ? sps->level_idc : -1,
               sps ? sps->ref_frame_count : -1, (unsigned)hardware->cabac,
               (unsigned)hardware->qscale, (unsigned)hardware->mb_width, (unsigned)hardware->mb_height,
               (unsigned long)hardware->bs_buffer, (unsigned long)hardware->bs_index,
               (unsigned long)hardware->bs_size_in_bits, (unsigned long)hardware->bs_rbsp_en,
               (unsigned long)codec->h264.vpu_ctx.sch_stat, (unsigned long)codec->h264.vpu_ctx.sde_stat);
        if (hardware->bs_buffer && hardware->cabac) {
            const volatile uint8_t *bytes = (const volatile uint8_t *)(uintptr_t)
                                           (hardware->bs_buffer | 0xa0000000u);
            printf("CODEC_ERROR_BS first32=");
            for (unsigned index = 0; index < 32; ++index) printf("%02x", bytes[index]);
            printf("\n");
        }
        return result;
    }
    return result == (int)length ? 0 : -EIO;
}

int player_codec_drain(player_codec_t *codec, uint32_t session)
{
    AVPacket packet = { 0 };
    int got = 0, result;
    if (!valid_session(codec, session)) return -ESTALE;
    result = h264_decode_frame(&codec->avctx, NULL, &got, &packet);
    return result < 0 ? result : got;
}

int player_codec_get_ready(player_codec_t *codec, uint32_t session, player_codec_frame_t *result)
{
    AVFrame *frame;
    struct player_codec_slot *slot;
    if (!valid_session(codec, session)) return -ESTALE;
    if (!result) return -EINVAL;
    frame = h264_dequeue_frame(&codec->h264);
    if (!frame) return -EAGAIN;
    slot = frame->opaque;
    result->token = slot->token;
    result->y = slot->planes[0].buffer; result->uv = slot->planes[1].buffer;
    result->physical_y = slot->planes[0].buffer_pa; result->physical_uv = slot->planes[1].buffer_pa;
    result->width = codec->layout.width; result->height = codec->layout.height; result->stride = codec->layout.stride;
    result->frame_index = slot->frame_index; result->key_frame = (uint32_t)frame->key_frame;
    result->output_index = codec->output_count++;
    result->pts_us = slot->pts_us; result->duration_us = slot->duration_us;
    return 0;
}

int player_codec_return_frame(player_codec_t *codec, player_video_token_t token)
{
    if (!valid_session(codec, token.session)) return -ESTALE;
    return player_video_discard_ready(codec->pool, token) ? 0 : -EINVAL;
}

int player_codec_stop(player_codec_t *codec, uint32_t session)
{
    uint32_t i;
    int result;
    if (!valid_session(codec, session)) return -ESTALE;
    result = player_felix_stop(&codec->h264.vpu_ctx);
    if (result) return result;
    h264_decode_end(&codec->avctx);
    for (i = 0; i < PLAYER_VIDEO_SLOTS; ++i)
        (void)player_video_discard_ready(codec->pool, codec->slots[i].token);
    codec->running = 0;
    return 0;
}

void player_codec_close(player_codec_t *codec)
{
    if (!codec) return;
    if (codec->running && player_codec_stop(codec, codec->session)) return;
    if (active_codec == codec) active_codec = NULL;
    free(codec);
}
