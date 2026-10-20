#ifndef PLAYER_MP4_H
#define PLAYER_MP4_H

#include "player_media.h"
#include <stdint.h>

#define PLAYER_MP4_CONFIG_MAX 4096u
#define PLAYER_MP4_AUDIO_MAX (64u * 1024u)

struct player_mp4;

enum player_mp4_codec { PLAYER_MP4_H264 = 1, PLAYER_MP4_AAC_LC = 2 };
enum player_mp4_packet_flags {
    PLAYER_MP4_PACKET_SYNC = 1u,
    PLAYER_MP4_PACKET_IDR = 2u
};

/* Times are deliberately kept in their original integer timebases. All
 * source samples, including encoder priming and end padding, are returned.
 * An edit clips [edit_media_time, edit_media_time + edit_duration converted
 * to track ticks). An optional empty edit delays that interval. The caller
 * applies the clip on decoded source samples, accounting separately for any
 * samples already skipped by its decoder. No AAC delay is guessed here. */
struct player_mp4_track_info {
    enum player_mp4_codec codec;
    uint32_t track_id, timescale, sample_count;
    uint64_t duration;           /* Sum of stts, in track ticks. */
    uint64_t header_duration;    /* mdhd duration, in track ticks. */
    uint32_t width, height;
    /* avc1 sample-entry fallback only; SPS values take precedence. ISO colour
     * values 1=709, 5/6=601, 2=unknown. Other explicit ISO values are retained
     * for capability rejection, never mapped to inferred 709. No tone mapping. */
    uint32_t matrix_coefficients, colour_primaries, transfer_characteristics;
    int32_t full_range;          /* -1 unknown, 0 limited, 1 full. */
    uint32_t sar_num, sar_den;   /* pasp; 0/0 means absent. */
    uint32_t sample_rate, channels, aac_frame_samples;
    uint32_t h264_profile, h264_level, nal_length_bytes;
    uint32_t has_ctts, has_edit;
    int64_t edit_media_time;     /* Track ticks, >= 0 for the supported edit. */
    uint64_t empty_edit_duration;/* Movie ticks, before the media edit. */
    uint64_t edit_duration;      /* Movie ticks; meaningful when has_edit. */
    int64_t presentation_start_us, presentation_end_us; /* Edited window. */
    int64_t presentation_last_pts_us; /* Last picture intersecting the window. */
    uint32_t presentation_samples;   /* Visible pictures; video only. */
    const uint8_t *config;       /* avcC or AudioSpecificConfig; owned by demux. */
    uint32_t config_bytes;
};

struct player_mp4_info {
    uint32_t movie_timescale;
    uint64_t movie_duration;
    uint32_t has_video, has_audio;
    struct player_mp4_track_info video, audio;
};

struct player_mp4_packet {
    enum player_packet_kind kind;
    uint32_t sample_index, flags;
    uint64_t source_offset;
    uint32_t source_bytes;       /* File bytes, before AVCC normalization. */
    uint32_t normalized_bytes;   /* Exact output bytes, excluding 64-byte pad. */
    int64_t dts, pts;            /* Original track ticks, before edit mapping. */
    uint32_t timescale, duration;
    int64_t dts_us, pts_us;      /* Edit-mapped times; preroll may be negative. */
    uint64_t duration_us;
};

/* Nonfragmented, unencrypted avc1 H264 and/or mp4a AAC-LC only. Metadata does
 * not certify the hardware decoder's profile/reference/geometry capability.
 * Tables are read through bounded page caches; the file and IO outlive ctx.
 * open/next/read are synchronous, from the single file-owning worker only.
 * Results reuse player_media_result. No ownership of IO is transferred. */
int player_mp4_open(struct player_mp4 **out, const struct player_media_io *io);
const struct player_mp4_info *player_mp4_get_info(const struct player_mp4 *ctx);
int player_mp4_next(struct player_mp4 *ctx, struct player_mp4_packet *packet);
/* AAC returns one raw AU. H264 returns one Annex B AU with an AUD, and
 * avcC SPS/PPS before an IDR. Capacity includes INPUT_PADDING; written excludes
 * padding and always equals packet.normalized_bytes on success. */
int player_mp4_read_packet(struct player_mp4 *ctx,
                           const struct player_mp4_packet *packet,
                           void *buffer, uint32_t capacity, uint32_t *written);
void player_mp4_close(struct player_mp4 *ctx);

#endif
