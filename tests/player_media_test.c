#include "player_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define file_seek _fseeki64
#define file_tell _ftelli64
#else
#define file_seek fseeko
#define file_tell ftello
#endif

static int32_t host_read(void *opaque, void *buffer, uint32_t bytes)
{
    /* Small reads also exercise the firmware callback's short-read handling. */
    if (bytes > 997) bytes = 997;
    return (int32_t)fread(buffer, 1, bytes, opaque);
}
static int host_seek(void *opaque, uint64_t offset) { return file_seek(opaque, (int64_t)offset, SEEK_SET); }
static uint64_t host_size(void *opaque)
{
    FILE *file = opaque;
    int64_t saved = file_tell(file), size;
    file_seek(file, 0, SEEK_END); size = file_tell(file);
    file_seek(file, saved, SEEK_SET);
    return (uint64_t)size;
}

int main(int argc, char **argv)
{
    FILE *file;
    struct player_media media;
    struct player_media_packet packet;
    struct player_media_io io;
    uint8_t *buffer;
    uint64_t frames = 0, samples = 0;
    uint32_t max_packet = 0;
    int result;
    if (argc != 2 || !(file = fopen(argv[1], "rb"))) return 2;
    io.opaque = file; io.read = host_read; io.seek = host_seek; io.size = host_size;
    result = player_media_open(&media, &io);
    if (result) goto failed;
    buffer = malloc(PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
    if (!buffer) { fclose(file); return 2; }
    while ((result = player_media_next(&media, &packet)) == PLAYER_MEDIA_OK) {
        if (packet.length > PLAYER_MEDIA_VIDEO_MAX) { result = PLAYER_MEDIA_UNSUPPORTED; break; }
        if (packet.kind == PLAYER_PACKET_VIDEO) {
            if (player_media_read_packet(&media, &packet, buffer, packet.length) != PLAYER_MEDIA_BUFFER_SMALL) {
                result = PLAYER_MEDIA_BAD_FORMAT; break;
            }
            memset(buffer + packet.length, 0xff, PLAYER_MEDIA_INPUT_PADDING);
        }
        result = player_media_read_packet(&media, &packet, buffer,
                                          PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
        if (result) break;
        if (packet.kind == PLAYER_PACKET_VIDEO) {
            uint32_t index;
            int64_t pts = (int64_t)(frames * 1000000u * media.fps_den / media.fps_num);
            int64_t end = (int64_t)((frames + 1u) * 1000000u * media.fps_den / media.fps_num);
            if (packet.index != frames || packet.pts_us != pts || packet.dts_us != pts ||
                packet.duration_us != end - pts || packet.audio_frames) {
                result = PLAYER_MEDIA_BAD_FORMAT; break;
            }
            for (index = 0; index < PLAYER_MEDIA_INPUT_PADDING; ++index)
                if (buffer[packet.length + index]) { result = PLAYER_MEDIA_BAD_FORMAT; break; }
            if (result) break;
            if (packet.length > max_packet) max_packet = packet.length;
            ++frames;
        } else {
            int64_t pts = (int64_t)(samples * 1000000u / media.sample_rate);
            int64_t end = (int64_t)((samples + packet.length / 2u) * 1000000u / media.sample_rate);
            if (packet.index != samples || packet.pts_us != pts || packet.dts_us != pts ||
                packet.duration_us != end - pts || packet.audio_frames != packet.length / 2u) {
                result = PLAYER_MEDIA_BAD_FORMAT; break;
            }
            samples += packet.length / 2;
        }
    }
    free(buffer);
    if (result != PLAYER_MEDIA_EOF) goto failed;
    if (frames != media.video_frames || samples != media.audio_samples) {
        result = PLAYER_MEDIA_BAD_FORMAT; goto failed;
    }
    if (player_media_rewind(&media) != PLAYER_MEDIA_OK ||
        player_media_next(&media, &packet) != PLAYER_MEDIA_OK || packet.index != 0) {
        result = PLAYER_MEDIA_BAD_FORMAT; goto failed;
    }
    printf("{\"frames\":%llu,\"samples\":%llu,\"max_packet\":%u}\n",
           (unsigned long long)frames, (unsigned long long)samples, max_packet);
    fclose(file);
    return 0;
failed:
    fprintf(stderr, "%s (%d)\n", player_media_result_string(result), result);
    fclose(file);
    return 1;
}
