#include "player_source.h"
#include <limits.h>
#include <string.h>

static int at(player_source *s, uint64_t offset, void *out, uint32_t n)
{
    uint32_t done = 0;
    if (offset > s->end || n > s->end - offset) return PLAYER_MEDIA_BAD_FORMAT;
    if (s->io.seek(s->io.opaque, offset)) return PLAYER_MEDIA_IO_ERROR;
    while (done < n) {
        int32_t got = s->io.read(s->io.opaque, (uint8_t *)out + done, n - done);
        if (got <= 0 || (uint32_t)got > n - done) return PLAYER_MEDIA_IO_ERROR;
        done += (uint32_t)got;
    }
    return 0;
}

/* MPEG Layer III only; free-format has no bounded packet length here. */
static int mp3_header(const uint8_t *p, uint32_t *bytes, uint32_t *rate,
                       uint32_t *channels, uint32_t *frames)
{
    static const uint16_t br1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const uint32_t rates[3] = {44100,48000,32000};
    uint32_t version = (p[1] >> 3) & 3u, ri = (p[2] >> 2) & 3u;
    uint32_t bi = p[2] >> 4, kbps;
    if (p[0] != 255 || (p[1] & 0xe6) != 0xe2 || version == 1 || ri == 3 || !bi || bi == 15)
        return PLAYER_MEDIA_BAD_FORMAT;
    *rate = rates[ri] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    *channels = (p[3] >> 6) == 3 ? 1 : 2;
    *frames = version == 3 ? 1152 : 576;
    kbps = version == 3 ? br1[bi] : br2[bi];
    *bytes = (version == 3 ? 144000u : 72000u) * kbps / *rate + ((p[2] >> 1) & 1u);
    return 0;
}

static int adts_header(const uint8_t *p, uint32_t *bytes, uint32_t *rate,
                        uint32_t *channels, uint32_t *frames)
{
    static const uint32_t rates[13] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    uint32_t ri = (p[2] >> 2) & 15u;
    if (p[0] != 255 || (p[1] & 0xf6) != 0xf0 || ri >= 13) return PLAYER_MEDIA_BAD_FORMAT;
    *channels = ((p[2] & 1u) << 2) | (p[3] >> 6);
    if ((p[2] >> 6) != 1 || *channels < 1 || *channels > 2 ||
        rates[ri] < 8000 || rates[ri] > 48000 || (p[6] & 3u))
        return PLAYER_MEDIA_UNSUPPORTED;
    *rate = rates[ri]; *frames = 1024;
    *bytes = ((p[3] & 3u) << 11) | ((uint32_t)p[4] << 3) | (p[5] >> 5);
    return *bytes >= ((p[1] & 1u) ? 7u : 9u) ? 0 : PLAYER_MEDIA_BAD_FORMAT;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int mp3_gapless(player_source *s, uint32_t bytes, uint32_t frames)
{
    uint8_t b[2048];
    if (bytes > sizeof(b) || bytes > s->end - s->cursor) return PLAYER_MEDIA_BAD_FORMAT;
    int r = at(s, s->cursor, b, bytes);
    if (r) return r;
    uint32_t version = (b[1] >> 3) & 3u;
    uint32_t side = version == 3 ? (s->channels == 1 ? 17u : 32u) : (s->channels == 1 ? 9u : 17u);
    uint32_t pos = 4u + ((b[1] & 1u) ? 0u : 2u) + side;
    if (pos + 8u > bytes || (memcmp(b + pos, "Xing", 4) && memcmp(b + pos, "Info", 4))) return 0;
    uint32_t flags = be32(b + pos + 4), count = 0;
    pos += 8;
    if (flags & 1u) { if (pos + 4u > bytes) return PLAYER_MEDIA_BAD_FORMAT; count = be32(b + pos); pos += 4; }
    if (flags & 2u) pos += 4;
    if (flags & 4u) pos += 100;
    if (flags & 8u) pos += 4;
    if (pos > bytes) return PLAYER_MEDIA_BAD_FORMAT;
    if (count && pos + 24u <= bytes &&
        (!memcmp(b + pos, "LAME", 4) || !memcmp(b + pos, "Lavc", 4) || !memcmp(b + pos, "Lavf", 4))) {
        uint32_t delay = ((uint32_t)b[pos + 21] << 4) | (b[pos + 22] >> 4);
        uint32_t padding = ((b[pos + 22] & 15u) << 8) | b[pos + 23];
        uint64_t total = (uint64_t)count * frames;
        uint32_t tail = padding > 529u ? padding - 529u : 0;
        s->clip_start = delay + 529u;
        if (total < s->clip_start + tail) return PLAYER_MEDIA_BAD_FORMAT;
        s->clip_end = total - tail;
    }
    /* Metadata frame is outside the Xing frame count and must never enter
     * the core decoder. Its synthetic samples are not media PCM. */
    s->cursor += bytes;
    return 0;
}

static int video_geometry_supported(uint32_t width, uint32_t height)
{
    if (!width || !height || width > 2032u || height > 2032u || (width & 1u) || (height & 1u)) return 0;
    width = (width + 15u) & ~15u; height = (height + 15u) & ~15u;
    return (uint64_t)width * height <= 1280u * 720u;
}

static int mp4_info(player_source *s)
{
    const struct player_mp4_info *i = player_mp4_get_info(s->mp4);
    const struct player_mp4_track_info *a = &i->audio, *v = &i->video;
    if (!i->has_audio || a->sample_rate < 8000 || a->sample_rate > 48000 ||
        a->channels < 1 || a->channels > 2 || a->timescale != a->sample_rate ||
        a->empty_edit_duration || a->aac_frame_samples != 1024 ||
        (i->has_video && ((v->h264_profile != 66 && v->h264_profile != 77 && v->h264_profile != 100) ||
          v->empty_edit_duration || !v->presentation_samples ||
          !video_geometry_supported(v->width, v->height))))
        return PLAYER_MEDIA_UNSUPPORTED;
    s->kind = PLAYER_MEDIA_MP4;
    s->width = v->width; s->height = v->height;
    if (i->has_video) {
        s->video_matrix = v->matrix_coefficients; s->video_full_range = v->full_range;
        s->video_sar_num = v->sar_num; s->video_sar_den = v->sar_den;
        s->video_primaries = v->colour_primaries; s->video_transfer = v->transfer_characteristics;
    }
    s->video_frames = i->has_video ? v->sample_count : 0;
    s->video_display_frames = v->presentation_samples;
    s->video_start_us = v->presentation_start_us;
    s->video_end_us = v->presentation_end_us;
    s->video_last_pts_us = v->presentation_last_pts_us;
    s->source_rate = a->sample_rate; s->channels = a->channels; s->audio_codec = 3;
    s->audio_config = a->config; s->audio_config_bytes = a->config_bytes;
    s->clip_start = a->has_edit ? (uint64_t)a->edit_media_time : 0;
    s->clip_end = a->duration;
    if (a->has_edit) {
        if (!i->movie_timescale || a->edit_duration > UINT64_MAX / s->source_rate)
            return PLAYER_MEDIA_UNSUPPORTED;
        if (s->clip_start >= a->duration) return PLAYER_MEDIA_BAD_FORMAT;
        uint64_t length = a->edit_duration * s->source_rate / i->movie_timescale;
        if (length > UINT64_MAX - s->clip_start) return PLAYER_MEDIA_BAD_FORMAT;
        s->clip_end = s->clip_start + length;
        if (s->clip_end > a->duration) {
            /* elst uses movie ticks; an ordinary millisecond movie scale
             * can round the track end slightly upward. Intersect that last
             * tick with real media, without emitting encoder padding. */
            uint64_t tick = ((uint64_t)s->source_rate + i->movie_timescale - 1u) / i->movie_timescale;
            if (s->clip_end - a->duration > tick) return PLAYER_MEDIA_BAD_FORMAT;
            s->clip_end = a->duration;
        }
    }
    /* Actual decoder output seals the protocol at EOF, including native AAC
     * priming. Do not promise samples that the decoder may already suppress. */
    return 0;
}

int player_source_open(player_source *s, const struct player_media_io *io)
{
    uint8_t b[12]; int r;
    if (!s || !io || !io->read || !io->seek || !io->size) return PLAYER_MEDIA_IO_ERROR;
    memset(s, 0, sizeof(*s)); s->io = *io; s->end = io->size(io->opaque); s->clip_end = UINT64_MAX;
    s->video_matrix = 2u; s->video_full_range = -1;
    s->video_primaries = 2u; s->video_transfer = 2u;
    if (s->end < sizeof(b)) return PLAYER_MEDIA_BAD_FORMAT;
    if ((r = at(s, 0, b, sizeof(b))) != 0) return r;
    if (!memcmp(b, "RIFF", 4)) {
        r = player_media_open(&s->legacy, io);
        if (r) return r;
        s->kind = s->legacy.kind; s->width = s->legacy.width; s->height = s->legacy.height;
        s->video_frames = s->legacy.video_frames; s->audio_samples = s->legacy.audio_samples;
        s->video_display_frames = s->video_frames;
        if (s->video_frames) {
            s->video_matrix = 6u; s->video_full_range = 1;
            s->video_sar_num = 1u; s->video_sar_den = 1u;
            s->video_primaries = 6u; s->video_transfer = 6u;
            s->video_end_us = (int64_t)(s->video_frames * 1000000u * s->legacy.fps_den / s->legacy.fps_num);
            s->video_last_pts_us = (int64_t)((s->video_frames - 1u) * 1000000u * s->legacy.fps_den / s->legacy.fps_num);
        }
        s->source_rate = 48000; s->channels = 1;
        return 0;
    }
    if (!memcmp(b + 4, "ftyp", 4) || !memcmp(b + 4, "moov", 4) || !memcmp(b + 4, "free", 4)) {
        r = player_mp4_open(&s->mp4, io);
        if (!r) r = mp4_info(s);
        if (r) player_source_close(s);
        return r;
    }
    if (!memcmp(b, "ID3", 3)) {
        uint8_t flags = b[3] == 2 ? 0xc0u : b[3] == 3 ? 0xe0u : 0xf0u;
        if (b[3] < 2 || b[3] > 4 || b[4] == 0xffu || (b[5] & ~flags) ||
            (b[6] | b[7] | b[8] | b[9]) & 0x80)
            return PLAYER_MEDIA_BAD_FORMAT;
        s->cursor = 10u + ((uint32_t)b[6] << 21) + ((uint32_t)b[7] << 14) +
                    ((uint32_t)b[8] << 7) + b[9] + ((b[3] == 4 && (b[5] & 16u)) ? 10u : 0u);
        if (b[3] == 4 && (b[5] & 16u)) {
            uint8_t footer[10];
            if ((r = at(s, s->cursor - 10u, footer, sizeof(footer)))) return r;
            if (memcmp(footer, "3DI", 3) || memcmp(footer + 3, b + 3, 7))
                return PLAYER_MEDIA_BAD_FORMAT;
        }
    }
    if (s->cursor > s->end || s->end - s->cursor < 7) return PLAYER_MEDIA_BAD_FORMAT;
    if ((r = at(s, s->cursor, b, 7))) return r;
    uint32_t bytes, frames;
    r = mp3_header(b, &bytes, &s->source_rate, &s->channels, &frames);
    if (!r) { s->kind = PLAYER_MEDIA_MP3; s->audio_codec = 1; }
    else {
        r = adts_header(b, &bytes, &s->source_rate, &s->channels, &frames);
        if (r) return r;
        s->kind = PLAYER_MEDIA_AAC; s->audio_codec = 2;
    }
    if (s->kind == PLAYER_MEDIA_MP3 && s->end >= 128) {
        if ((r = at(s, s->end - 128, b, 3))) return r;
        if (!memcmp(b, "TAG", 3)) s->end -= 128;
    }
    if (s->kind == PLAYER_MEDIA_MP3) return mp3_gapless(s, bytes, frames);
    return 0;
}

int player_source_next(player_source *s, struct player_media_packet *p)
{
    uint8_t b[7]; uint32_t rate, channels, frames, bytes; int r;
    memset(p, 0, sizeof(*p));
    if (!s->audio_codec) return player_media_next(&s->legacy, p);
    if (s->mp4) {
        r = player_mp4_next(s->mp4, &s->mp4_pending);
        if (r) return r;
        const struct player_mp4_packet *m = &s->mp4_pending;
        p->kind = m->kind; p->offset = m->source_offset; p->length = m->normalized_bytes;
        p->index = m->sample_index; p->pts_us = m->pts_us;
        p->dts_us = m->dts_us; p->duration_us = (int64_t)m->duration_us;
        p->audio_frames = m->kind == PLAYER_PACKET_AUDIO ? 1024 : 0;
        if (m->kind == PLAYER_PACKET_VIDEO && !m->duration_us)
            return PLAYER_MEDIA_UNSUPPORTED;
        return 0;
    }
    if (s->cursor == s->end) return PLAYER_MEDIA_EOF;
    if (s->end - s->cursor < 7) return PLAYER_MEDIA_BAD_FORMAT;
    if ((r = at(s, s->cursor, b, 7))) return r;
    r = s->kind == PLAYER_MEDIA_MP3 ? mp3_header(b, &bytes, &rate, &channels, &frames) :
                                    adts_header(b, &bytes, &rate, &channels, &frames);
    if (r) return r;
    if (rate != s->source_rate || channels != s->channels) return PLAYER_MEDIA_UNSUPPORTED;
    if (bytes > s->end - s->cursor) return PLAYER_MEDIA_BAD_FORMAT;
    p->kind = PLAYER_PACKET_AUDIO; p->offset = s->cursor; p->length = bytes;
    p->index = s->raw_index; p->audio_frames = frames;
    p->pts_us = (int64_t)(s->raw_index / rate * 1000000u +
                         s->raw_index % rate * 1000000u / rate);
    p->dts_us = p->pts_us;
    p->duration_us = (int64_t)((s->raw_index + frames) / rate * 1000000u +
                              (s->raw_index + frames) % rate * 1000000u / rate) - p->pts_us;
    s->raw_index += frames; s->cursor += bytes;
    return 0;
}

int player_source_read(player_source *s, const struct player_media_packet *p,
                       void *out, uint32_t capacity)
{
    if (!s->audio_codec) return player_media_read_packet(&s->legacy, p, out, capacity);
    if (s->mp4) {
        uint32_t written;
        return player_mp4_read_packet(s->mp4, &s->mp4_pending, out, capacity, &written);
    }
    if (capacity < p->length) return PLAYER_MEDIA_BUFFER_SMALL;
    return at(s, p->offset, out, p->length);
}

void player_source_close(player_source *s)
{
    if (s->mp4) player_mp4_close(s->mp4);
    s->mp4 = NULL;
}

int player_source_clip_video(const player_source *s, int64_t *pts, int64_t *duration)
{
    if (!s || !pts || !duration || *duration <= 0 || *pts > INT64_MAX - *duration)
        return PLAYER_MEDIA_BAD_FORMAT;
    int64_t end = *pts + *duration;
    if (end <= s->video_start_us || *pts >= s->video_end_us) return 0;
    if (*pts < s->video_start_us) *pts = s->video_start_us;
    if (end > s->video_end_us) end = s->video_end_us;
    *duration = end - *pts;
    return 1;
}
