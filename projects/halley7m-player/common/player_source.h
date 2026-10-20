#ifndef PLAYER_SOURCE_H
#define PLAYER_SOURCE_H

#include "player_media.h"
#include "player_mp4.h"

/* Unified streaming source. Only this object owns its MP4 table cursor;
 * queued packet descriptors contain no private decoder pointers. */
typedef struct player_source {
    struct player_media legacy;
    struct player_media_io io;
    struct player_mp4 *mp4;
    struct player_mp4_packet mp4_pending;
    enum player_media_kind kind;
    uint32_t width, height, source_rate, channels, audio_codec;
    /* Container fallback; fill only unknown SPS properties, then infer at
     * display as a last resort. Legacy AVI retains its 601/full contract. */
    uint32_t video_matrix, video_sar_num, video_sar_den;
    uint32_t video_primaries, video_transfer; /* ISO, 2 unspecified. */
    int32_t video_full_range; /* -1 unknown, 0 limited, 1 full. */
    uint64_t video_frames, audio_samples; /* 48k output; zero = unknown. */
    uint64_t video_display_frames; /* Visible outputs; decode all video_frames AUs. */
    int64_t video_start_us, video_end_us, video_last_pts_us;
    uint64_t clip_start, clip_end;        /* Source PCM frame axis. */
    const uint8_t *audio_config;
    uint32_t audio_config_bytes;
    uint64_t cursor, end, raw_index;
    uint32_t first_frame;
} player_source;

int player_source_open(player_source *source, const struct player_media_io *io);
/* next/read are synchronous from one owner. Read or discard the current
 * packet before calling next again; MP4 normalization uses the current
 * internal cursor. Queue owned bytes after read, never a deferred source
 * descriptor. Compressed input capacity should include INPUT_PADDING. */
int player_source_next(player_source *source, struct player_media_packet *packet);
int player_source_read(player_source *source, const struct player_media_packet *packet,
                       void *buffer, uint32_t capacity);
void player_source_close(player_source *source);
/* Intersect a decoded picture with the edited presentation window. Returns
 * 1 if visible (and clips PTS/duration), 0 if wholly outside, negative on an
 * invalid interval. Keep the original PTS for comparison with last_pts_us. */
int player_source_clip_video(const player_source *source, int64_t *pts_us,
                             int64_t *duration_us);

#endif
