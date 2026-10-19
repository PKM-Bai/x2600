#ifndef PLAYER_MEDIA_H
#define PLAYER_MEDIA_H

#include <stdint.h>

#define PLAYER_MEDIA_VIDEO_MAX (512u * 1024u)
#define PLAYER_MEDIA_INPUT_PADDING 64u
#define PLAYER_MEDIA_AUDIO_PACKET 4096u
#define PLAYER_MEDIA_STACK_MAX 8u

enum player_media_result {
    PLAYER_MEDIA_OK = 0,
    PLAYER_MEDIA_EOF = 1,
    PLAYER_MEDIA_IO_ERROR = -1,
    PLAYER_MEDIA_BAD_FORMAT = -2,
    PLAYER_MEDIA_UNSUPPORTED = -3,
    PLAYER_MEDIA_BUFFER_SMALL = -4
};

enum player_media_kind { PLAYER_MEDIA_WAV = 1, PLAYER_MEDIA_AVI = 2 };
enum player_packet_kind { PLAYER_PACKET_VIDEO = 1, PLAYER_PACKET_AUDIO = 2 };

/* read returns the byte count (negative on error); seek returns 0 on success. */
struct player_media_io {
    void *opaque;
    int32_t (*read)(void *opaque, void *buffer, uint32_t bytes);
    int (*seek)(void *opaque, uint64_t absolute);
    uint64_t (*size)(void *opaque);
};

struct player_media_packet {
    enum player_packet_kind kind;
    uint32_t length;             /* File bytes only: excludes RIFF and decoder padding. */
    uint64_t offset;
    uint64_t index;              /* Video frame number or first PCM sample number. */
};

struct player_media {
    struct player_media_io io;
    enum player_media_kind kind;
    uint64_t file_size, next_offset, data_start, data_end;
    uint64_t container_end[PLAYER_MEDIA_STACK_MAX];
    uint64_t video_frames, audio_samples;
    uint64_t next_video_frame, next_audio_sample;
    uint32_t width, height, fps_num, fps_den;
    uint32_t sample_rate, audio_block_align;
    uint8_t video_stream, audio_stream, depth;
};

/* AVI contract: H264 Annex B, 720x1280 CFR30, PCM S16LE/48000/mono. */
int player_media_open(struct player_media *media, const struct player_media_io *io);
int player_media_rewind(struct player_media *media);
int player_media_next(struct player_media *media, struct player_media_packet *packet);
/* Video capacity must include INPUT_PADDING; padding is always zeroed. */
int player_media_read_packet(struct player_media *media,
                             const struct player_media_packet *packet,
                             void *buffer, uint32_t capacity);
/* Checks one complete AU. The first must contain SPS/PPS before an IDR. */
int player_media_validate_h264(const uint8_t *data, uint32_t length, int first);
const char *player_media_result_string(int result);

#endif
