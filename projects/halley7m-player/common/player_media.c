#include "player_media.h"

#include <string.h>

#define TAG(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define RIFF TAG('R','I','F','F')
#define LIST TAG('L','I','S','T')
#define AVI TAG('A','V','I',' ')
#define AVIX TAG('A','V','I','X')
#define WAVE TAG('W','A','V','E')
#define MOVI TAG('m','o','v','i')
#define REC TAG('r','e','c',' ')

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static int read_at(struct player_media *m, uint64_t offset, void *buffer, uint32_t bytes)
{
    uint32_t done = 0;
    if (offset > m->file_size || bytes > m->file_size - offset)
        return PLAYER_MEDIA_BAD_FORMAT;
    if (m->io.seek(m->io.opaque, offset)) return PLAYER_MEDIA_IO_ERROR;
    while (done < bytes) {
        int32_t got = m->io.read(m->io.opaque, (uint8_t *)buffer + done, bytes - done);
        if (got <= 0 || (uint32_t)got > bytes - done) return PLAYER_MEDIA_IO_ERROR;
        done += (uint32_t)got;
    }
    return PLAYER_MEDIA_OK;
}

static int chunk(struct player_media *m, uint64_t pos, uint64_t limit,
                 uint32_t *tag, uint32_t *length, uint64_t *after)
{
    uint8_t b[8];
    uint64_t end;
    int result;
    if (pos > limit || limit - pos < 8) return PLAYER_MEDIA_BAD_FORMAT;
    result = read_at(m, pos, b, sizeof(b));
    if (result) return result;
    *tag = le32(b); *length = le32(b + 4);
    end = pos + 8 + (uint64_t)*length;
    *after = end + (*length & 1u);
    if (*after > limit || *after > m->file_size) return PLAYER_MEDIA_BAD_FORMAT;
    return PLAYER_MEDIA_OK;
}

static int pcm_format(struct player_media *m, const uint8_t *b, uint32_t size)
{
    if (size < 16) return PLAYER_MEDIA_BAD_FORMAT;
    if (le16(b) != 1 || le16(b + 2) != 1 || le32(b + 4) != 48000 ||
        le32(b + 8) != 96000 || le16(b + 12) != 2 || le16(b + 14) != 16)
        return PLAYER_MEDIA_UNSUPPORTED;
    m->sample_rate = 48000; m->audio_block_align = 2;
    return PLAYER_MEDIA_OK;
}

static int parse_stream(struct player_media *m, uint64_t pos, uint64_t end, uint8_t index)
{
    uint8_t b[56];
    uint32_t tag, len, type = 0, scale = 0, rate = 0, count = 0;
    int have_format = 0, result;
    uint64_t after;
    while (pos < end) {
        result = chunk(m, pos, end, &tag, &len, &after);
        if (result) return result;
        if (tag == TAG('s','t','r','h')) {
            if (len < sizeof(b)) return PLAYER_MEDIA_BAD_FORMAT;
            if ((result = read_at(m, pos + 8, b, sizeof(b)))) return result;
            type = le32(b); scale = le32(b + 20); rate = le32(b + 24);
            count = le32(b + 32);
            if (le32(b + 28) != 0 || !scale) return PLAYER_MEDIA_UNSUPPORTED;
        } else if (tag == TAG('s','t','r','f')) {
            uint32_t needed = type == TAG('v','i','d','s') ? 40u : 16u;
            if (len < needed || !type) return PLAYER_MEDIA_BAD_FORMAT;
            if ((result = read_at(m, pos + 8, b, needed))) return result;
            if (type == TAG('v','i','d','s')) {
                if (m->video_stream != 255 || (uint64_t)scale * 30 != rate ||
                    le32(b) < 40 || le32(b + 4) != 720 || le32(b + 8) != 1280 ||
                    le32(b + 16) != TAG('H','2','6','4')) return PLAYER_MEDIA_UNSUPPORTED;
                m->video_stream = index; m->width = 720; m->height = 1280;
                m->fps_num = 30; m->fps_den = 1; m->video_frames = count;
            } else if (type == TAG('a','u','d','s')) {
                if (m->audio_stream != 255 || (uint64_t)scale * 48000 != rate)
                    return PLAYER_MEDIA_UNSUPPORTED;
                if ((result = pcm_format(m, b, len))) return result;
                m->audio_stream = index; m->audio_samples = count;
            } else return PLAYER_MEDIA_UNSUPPORTED;
            have_format = 1;
        }
        pos = after;
    }
    return have_format ? PLAYER_MEDIA_OK : PLAYER_MEDIA_BAD_FORMAT;
}

static int parse_hdrl(struct player_media *m, uint64_t pos, uint64_t end)
{
    uint32_t tag, len;
    uint8_t b[4], stream = 0;
    uint64_t after;
    int result;
    while (pos < end) {
        if ((result = chunk(m, pos, end, &tag, &len, &after))) return result;
        if (tag == LIST && len >= 4) {
            if ((result = read_at(m, pos + 8, b, 4))) return result;
            if (le32(b) == TAG('s','t','r','l')) {
                if (stream == 100) return PLAYER_MEDIA_UNSUPPORTED;
                if ((result = parse_stream(m, pos + 12, pos + 8 + len, stream))) return result;
                ++stream;
            }
        }
        pos = after;
    }
    return PLAYER_MEDIA_OK;
}

int player_media_open(struct player_media *m, const struct player_media_io *io)
{
    uint8_t b[16];
    uint32_t tag, len, form;
    uint64_t pos, end, after;
    int result, have_header = 0, have_pcm = 0;
    if (!m || !io || !io->read || !io->seek || !io->size) return PLAYER_MEDIA_IO_ERROR;
    memset(m, 0, sizeof(*m)); m->io = *io;
    m->video_stream = m->audio_stream = 255;
    m->file_size = io->size(io->opaque);
    if (m->file_size >= UINT64_C(0x100000000)) return PLAYER_MEDIA_UNSUPPORTED;
    if ((result = read_at(m, 0, b, 12))) return result;
    if (le32(b) != RIFF || le32(b + 4) < 4) return PLAYER_MEDIA_BAD_FORMAT;
    form = le32(b + 8); end = 8 + (uint64_t)le32(b + 4);
    if (end > m->file_size) return PLAYER_MEDIA_BAD_FORMAT;
    if (form != AVI && form != WAVE) return PLAYER_MEDIA_UNSUPPORTED;
    m->kind = form == AVI ? PLAYER_MEDIA_AVI : PLAYER_MEDIA_WAV;
    for (pos = 12; pos < end; pos = after) {
        if ((result = chunk(m, pos, end, &tag, &len, &after))) return result;
        if (m->kind == PLAYER_MEDIA_WAV) {
            if (tag == TAG('f','m','t',' ')) {
                if (len < 16) return PLAYER_MEDIA_BAD_FORMAT;
                if ((result = read_at(m, pos + 8, b, 16))) return result;
                if ((result = pcm_format(m, b, len))) return result;
                have_pcm = 1;
            } else if (tag == TAG('d','a','t','a')) {
                if (!have_pcm || (len & 1)) return PLAYER_MEDIA_BAD_FORMAT;
                m->data_start = m->next_offset = pos + 8;
                m->data_end = pos + 8 + len; m->audio_samples = len / 2;
                return PLAYER_MEDIA_OK;
            }
        } else if (tag == LIST) {
            if (len < 4) return PLAYER_MEDIA_BAD_FORMAT;
            if ((result = read_at(m, pos + 8, b, 4))) return result;
            if (le32(b) == TAG('h','d','r','l')) {
                if ((result = parse_hdrl(m, pos + 12, pos + 8 + len))) return result;
                have_header = 1;
            } else if (le32(b) == MOVI) {
                if (!have_header || m->video_stream == 255 || m->audio_stream == 255 || !m->video_frames)
                    return PLAYER_MEDIA_BAD_FORMAT;
                m->data_start = m->next_offset = pos + 12;
                m->data_end = pos + 8 + len;
                m->container_end[0] = end; m->container_end[1] = m->data_end;
                m->depth = 2;
                return PLAYER_MEDIA_OK;
            }
        }
    }
    return PLAYER_MEDIA_BAD_FORMAT;
}

int player_media_rewind(struct player_media *m)
{
    struct player_media_io io = m->io;
    return player_media_open(m, &io);
}

int player_media_next(struct player_media *m, struct player_media_packet *packet)
{
    uint32_t tag, len, form;
    uint64_t after, pos;
    uint8_t b[4];
    int result;
    if (m->kind == PLAYER_MEDIA_WAV) {
        if (m->next_offset == m->data_end) return PLAYER_MEDIA_EOF;
        packet->kind = PLAYER_PACKET_AUDIO; packet->offset = m->next_offset;
        packet->length = m->data_end - m->next_offset > PLAYER_MEDIA_AUDIO_PACKET ?
                         PLAYER_MEDIA_AUDIO_PACKET : (uint32_t)(m->data_end - m->next_offset);
        packet->index = m->next_audio_sample;
        m->next_offset += packet->length; m->next_audio_sample += packet->length / 2;
        return PLAYER_MEDIA_OK;
    }
    if (m->kind != PLAYER_MEDIA_AVI) return PLAYER_MEDIA_BAD_FORMAT;
    for (;;) {
        pos = m->next_offset;
        while (m->depth && pos == m->container_end[m->depth - 1]) {
            pos += pos & 1u; --m->depth;
        }
        if (!m->depth) {
            if (pos == m->file_size)
                return m->next_video_frame == m->video_frames && m->next_audio_sample == m->audio_samples ?
                       PLAYER_MEDIA_EOF : PLAYER_MEDIA_BAD_FORMAT;
            if ((result = chunk(m, pos, m->file_size, &tag, &len, &after))) return result;
            if (tag != RIFF || len < 4) return PLAYER_MEDIA_BAD_FORMAT;
            if ((result = read_at(m, pos + 8, b, 4))) return result;
            if (le32(b) != AVIX) return PLAYER_MEDIA_UNSUPPORTED;
            m->container_end[0] = pos + 8 + len; m->depth = 1;
            m->next_offset = pos + 12;
            continue;
        }
        if ((result = chunk(m, pos, m->container_end[m->depth - 1], &tag, &len, &after))) return result;
        m->next_offset = after;
        if (tag == LIST) {
            if (len < 4) return PLAYER_MEDIA_BAD_FORMAT;
            if ((result = read_at(m, pos + 8, b, 4))) return result;
            form = le32(b);
            if (form == MOVI || form == REC) {
                if (m->depth == PLAYER_MEDIA_STACK_MAX) return PLAYER_MEDIA_BAD_FORMAT;
                m->container_end[m->depth++] = pos + 8 + len;
                m->next_offset = pos + 12;
            }
            continue;
        }
        if (m->depth < 2) continue;
        b[0] = (uint8_t)tag; b[1] = (uint8_t)(tag >> 8);
        if (b[0] < '0' || b[0] > '9' || b[1] < '0' || b[1] > '9') continue;
        form = (uint32_t)(b[0] - '0') * 10 + b[1] - '0';
        if (form == m->video_stream && (tag >> 16) == ('d' | ('c' << 8))) {
            if (!len || len > PLAYER_MEDIA_VIDEO_MAX) return PLAYER_MEDIA_UNSUPPORTED;
            packet->kind = PLAYER_PACKET_VIDEO; packet->index = m->next_video_frame++;
        } else if (form == m->audio_stream && (tag >> 16) == ('w' | ('b' << 8))) {
            if (len & 1) return PLAYER_MEDIA_BAD_FORMAT;
            if (!len) continue;
            packet->kind = PLAYER_PACKET_AUDIO; packet->index = m->next_audio_sample;
            m->next_audio_sample += len / 2;
        } else continue;
        packet->offset = pos + 8; packet->length = len;
        return PLAYER_MEDIA_OK;
    }
}

int player_media_validate_h264(const uint8_t *data, uint32_t length, int first)
{
    uint32_t i = 0, nals = 0, aud = 0, vcl = 0;
    int sps = 0, pps = 0, idr = 0;
    while (i + 3 < length) {
        uint32_t start = i, prefix = 0;
        if (data[i] == 0 && data[i+1] == 0) {
            if (data[i+2] == 1) prefix = 3;
            else if (data[i+2] == 0 && data[i+3] == 1) prefix = 4;
        }
        if (!prefix) { ++i; continue; }
        if (!nals && start != 0) return PLAYER_MEDIA_BAD_FORMAT;
        i += prefix;
        if (i >= length || (data[i] & 0x80)) return PLAYER_MEDIA_BAD_FORMAT;
        switch (data[i] & 31) {
        case 7: sps = 1; break;
        case 8: pps = 1; break;
        case 9: ++aud; break;
        case 5:
            if (first && (!sps || !pps)) return PLAYER_MEDIA_BAD_FORMAT;
            idr = 1;
            /* fall through */
        case 1: ++vcl; break;
        default: break;
        }
        ++nals; ++i;
    }
    if (!nals || aud != 1 || !vcl || (first && (!sps || !pps || !idr)))
        return PLAYER_MEDIA_BAD_FORMAT;
    return PLAYER_MEDIA_OK;
}

int player_media_read_packet(struct player_media *m, const struct player_media_packet *packet,
                             void *buffer, uint32_t capacity)
{
    uint32_t padding = packet->kind == PLAYER_PACKET_VIDEO ? PLAYER_MEDIA_INPUT_PADDING : 0;
    int result;
    if (packet->length > capacity || padding > capacity - packet->length)
        return PLAYER_MEDIA_BUFFER_SMALL;
    if ((result = read_at(m, packet->offset, buffer, packet->length))) return result;
    if (padding) {
        memset((uint8_t *)buffer + packet->length, 0, padding);
        return player_media_validate_h264(buffer, packet->length, packet->index == 0);
    }
    return PLAYER_MEDIA_OK;
}

const char *player_media_result_string(int result)
{
    switch (result) {
    case PLAYER_MEDIA_OK: return "ok";
    case PLAYER_MEDIA_EOF: return "end of media";
    case PLAYER_MEDIA_IO_ERROR: return "media I/O error";
    case PLAYER_MEDIA_BAD_FORMAT: return "invalid media format";
    case PLAYER_MEDIA_UNSUPPORTED: return "unsupported media parameters";
    case PLAYER_MEDIA_BUFFER_SMALL: return "packet buffer too small";
    default: return "unknown media error";
    }
}
