/* Production unified source + actual decoders, with tiny reads/PCM chunks. */
#include "player_source.h"
#include "player_audio_decode.h"
#include "player_pcm_convert.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct memory_file { uint8_t *data; uint64_t length, position; uint32_t read_limit; int fail; };
static int32_t memory_read(void *opaque, void *data, uint32_t bytes)
{
    struct memory_file *f = opaque;
    if (f->fail) return -1;
    if (f->read_limit && bytes > f->read_limit) bytes = f->read_limit;
    if (bytes > f->length - f->position) bytes = (uint32_t)(f->length - f->position);
    memcpy(data, f->data + (size_t)f->position, bytes); f->position += bytes;
    return (int32_t)bytes;
}
static int memory_seek(void *opaque, uint64_t position)
{
    struct memory_file *f = opaque;
    if (position > f->length || f->fail) return -1;
    f->position = position; return 0;
}
static uint64_t memory_size(void *opaque) { return ((struct memory_file *)opaque)->length; }
static struct player_media_io memory_io(struct memory_file *f)
{
    struct player_media_io io = {f, memory_read, memory_seek, memory_size}; return io;
}
static void mp3_frame(uint8_t *data)
{
    memset(data, 0, 417); data[0] = 255; data[1] = 0xfb; data[2] = 0x90;
}
static void self_test(void)
{
    uint8_t bytes[1200] = {0}, payload[512]; player_source s; struct player_media_packet p;
    struct memory_file f = {bytes, 834, 0, 3, 0}; struct player_media_io io = memory_io(&f);
    mp3_frame(bytes); mp3_frame(bytes + 417);
    assert(!player_source_open(&s, &io));
    assert(s.kind == PLAYER_MEDIA_MP3 && s.source_rate == 44100 && s.channels == 2);
    assert(!player_source_next(&s, &p) && p.length == 417 && p.index == 0);
    assert(player_source_read(&s, &p, payload, 416) == PLAYER_MEDIA_BUFFER_SMALL);
    assert(!player_source_read(&s, &p, payload, sizeof(payload)) && !memcmp(payload, bytes, 417));
    f.fail = 1; assert(player_source_read(&s, &p, payload, sizeof(payload)) == PLAYER_MEDIA_IO_ERROR);
    f.fail = 0; assert(!player_source_next(&s, &p) && p.index == 1152);
    assert(player_source_next(&s, &p) == PLAYER_MEDIA_EOF);
    assert(player_source_next(&s, &p) == PLAYER_MEDIA_EOF); player_source_close(&s);
    f.fail = 1; assert(player_source_open(&s, &io) == PLAYER_MEDIA_IO_ERROR); f.fail = 0;
    f.length = 427; assert(!player_source_open(&s, &io));
    assert(!player_source_next(&s, &p)); assert(player_source_next(&s, &p) == PLAYER_MEDIA_BAD_FORMAT);
    player_source_close(&s);
    f.length = 834; bytes[419] = 0x94;
    assert(!player_source_open(&s, &io)); assert(!player_source_next(&s, &p));
    assert(player_source_next(&s, &p) == PLAYER_MEDIA_UNSUPPORTED); player_source_close(&s); bytes[419] = 0x90;
    /* ID3v1 only applies to MP3, not an incidental TAG inside AAC payload. */
    f.length = 962; memcpy(bytes + 834, "TAG", 3);
    assert(!player_source_open(&s, &io) && s.end == 834); player_source_close(&s);
    memset(bytes, 0, sizeof(bytes)); f.length = 154;
    bytes[0] = 255; bytes[1] = 0xf1; bytes[2] = 0x4c; bytes[3] = 0x80;
    bytes[4] = 19; bytes[5] = 0x5f; bytes[6] = 0xfc; memcpy(bytes + 26, "TAG", 3);
    assert(!player_source_open(&s, &io) && s.end == 154);
    assert(!player_source_next(&s, &p) && p.length == 154);
    assert(player_source_next(&s, &p) == PLAYER_MEDIA_EOF); player_source_close(&s);
    bytes[2] = 0x70; assert(player_source_open(&s, &io) == PLAYER_MEDIA_UNSUPPORTED); /* 7350Hz */
    bytes[2] = 0x7c; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT); /* index15 */
    /* v2.4 footer size excludes both ten-byte header and footer. */
    memset(bytes, 0, sizeof(bytes)); memcpy(bytes, "ID3\4\0\20", 6); bytes[9] = 13;
    memcpy(bytes + 23, "3DI", 3); memcpy(bytes + 26, bytes + 3, 7);
    mp3_frame(bytes + 33); mp3_frame(bytes + 450); f.length = 867;
    assert(!player_source_open(&s, &io) && s.cursor == 33); player_source_close(&s);
    bytes[23] = 'X'; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT); bytes[23] = '3';
    bytes[32] = 12; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT); bytes[32] = 13;
    bytes[3] = 3; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT); bytes[3] = 4;
    bytes[6] = 128; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT); bytes[6] = 0;
    bytes[9] = 127; f.length = 40; assert(player_source_open(&s, &io) == PLAYER_MEDIA_BAD_FORMAT);
    memset(&s,0,sizeof(s)); s.video_end_us=1000000;
    int64_t pts=-33333,duration=33333;
    assert(!player_source_clip_video(&s,&pts,&duration));
    pts=-10000; duration=33333;
    assert(player_source_clip_video(&s,&pts,&duration)==1 && !pts && duration==23333);
    pts=999999; duration=33333;
    assert(player_source_clip_video(&s,&pts,&duration)==1 && pts==999999 && duration==1);
    pts=1000000; duration=33333; assert(!player_source_clip_video(&s,&pts,&duration));
    duration=0; assert(player_source_clip_video(&s,&pts,&duration)==PLAYER_MEDIA_BAD_FORMAT);
    pts=INT64_MAX; duration=1;
    assert(player_source_clip_video(&s,&pts,&duration)==PLAYER_MEDIA_BAD_FORMAT);
    puts("{\"self_test\":\"short read, IO error, truncated frame, format change, ID3v1 scope, ID3v2.4 footer/reserved flags/bounds passed\"}");
}
static FILE *output_file(const char *prefix, const char *suffix)
{
    size_t n = strlen(prefix) + strlen(suffix) + 1; char *path = malloc(n); assert(path);
    snprintf(path, n, "%s%s", prefix, suffix); FILE *f = fopen(path, "wb"); free(path); assert(f); return f;
}
static void le32(FILE *f, uint32_t value)
{
    uint8_t b[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    assert(fwrite(b, 1, 4, f) == 4);
}
int main(int argc, char **argv)
{
    self_test();
    if (argc == 1) return 0;
    if (argc != 3 && argc != 4) return 2;
    FILE *input = fopen(argv[1], "rb"); assert(input && !fseek(input, 0, SEEK_END));
    long size = ftell(input); assert(size >= 0 && size < 8 * 1024 * 1024 && !fseek(input, 0, SEEK_SET));
    uint8_t *bytes = malloc((size_t)size + 1u); assert(bytes);
    assert(fread(bytes, 1, (size_t)size, input) == (size_t)size && !fclose(input));
    struct memory_file f = {bytes, (uint64_t)size, 0, argc == 4 ? 3u : 0u, 0};
    struct player_media_io io = memory_io(&f); player_source source;
    int r = player_source_open(&source, &io);
    if (r) { printf("{\"error\":%d,\"stage\":\"open\"}\n", r); free(bytes); return 3; }
    printf("{\"info\":{\"kind\":%u,\"codec\":%u,\"rate\":%u,\"channels\":%u,\"video_frames\":%" PRIu64 ",\"clip_start\":%" PRIu64 ",\"clip_end\":%" PRIu64 ",\"video_start_us\":%" PRId64 ",\"video_end_us\":%" PRId64 ",\"video_last_pts_us\":%" PRId64 ",\"video_display_frames\":%" PRIu64 "}}\n",
           (unsigned)source.kind, source.audio_codec, source.source_rate, source.channels,
           source.video_frames, source.clip_start, source.clip_end, source.video_start_us,
           source.video_end_us, source.video_last_pts_us, source.video_display_frames);
    assert(source.audio_codec);
    player_audio_decode_config config = {(enum player_audio_codec)source.audio_codec,
                                        source.audio_config, source.audio_config_bytes};
    player_audio_decoder *decoder = NULL; assert(!player_audio_decoder_create(&config, &decoder));
    player_pcm_converter converter; assert(!player_pcm_convert_init(&converter, source.source_rate, source.channels));
    FILE *bundle = output_file(argv[2], ".packets"), *native = output_file(argv[2], ".native.s16");
    FILE *mono = output_file(argv[2], ".mono.s16"), *video = output_file(argv[2], ".h264");
    FILE *asc = output_file(argv[2], ".asc");
    if (source.audio_config_bytes)
        assert(fwrite(source.audio_config, 1, source.audio_config_bytes, asc) == source.audio_config_bytes);
    assert(!fclose(asc));
    uint8_t *packet = malloc(PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING); assert(packet);
    uint64_t axis = 0, native_frames = 0, output_frames = 0; uint32_t count = 0, skipped = 0, video_count = 0;
    le32(bundle, 0); struct player_media_packet p;
    while (!(r = player_source_next(&source, &p))) {
        memset(packet, 0, PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
        assert(p.length <= PLAYER_MEDIA_VIDEO_MAX);
        assert(player_source_read(&source, &p, packet, p.length - 1u) == PLAYER_MEDIA_BUFFER_SMALL);
        r = player_source_read(&source, &p, packet, PLAYER_MEDIA_VIDEO_MAX + PLAYER_MEDIA_INPUT_PADDING);
        if (r) break;
        printf("{\"packet\":{\"kind\":%u,\"offset\":%" PRIu64 ",\"bytes\":%u,\"index\":%" PRIu64 ",\"frames\":%u,\"pts_us\":%" PRId64 ",\"dts_us\":%" PRId64 ",\"duration_us\":%" PRId64 "}}\n",
               (unsigned)p.kind, p.offset, p.length, p.index, p.audio_frames,
               p.pts_us, p.dts_us, p.duration_us);
        if (p.kind == PLAYER_PACKET_VIDEO) {
            int64_t pts=p.pts_us, duration=p.duration_us;
            int visible=player_source_clip_video(&source,&pts,&duration); assert(visible>=0);
            printf("{\"video_clip\":{\"index\":%" PRIu64 ",\"visible\":%d,\"pts_us\":%" PRId64 ",\"duration_us\":%" PRId64 "}}\n",
                   p.index,visible,pts,duration);
            assert(fwrite(packet, 1, p.length, video) == p.length); ++video_count; continue;
        }
        le32(bundle, p.length); assert(fwrite(packet, 1, p.length, bundle) == p.length); ++count;
        player_audio_decoded_frame decoded; r = player_audio_decoder_decode(decoder, packet, p.length, &decoded);
        if (r) break;
        assert(decoded.consumed_bytes == p.length && decoded.sample_rate == source.source_rate && decoded.channels == source.channels);
        axis += decoded.decoder_skipped_frames; skipped += decoded.decoder_skipped_frames;
        uint64_t end = axis + decoded.frames, first = axis > source.clip_start ? axis : source.clip_start;
        uint64_t last = end < source.clip_end ? end : source.clip_end;
        uint32_t keep = last > first ? (uint32_t)(last - first) : 0;
        const int16_t *pcm = decoded.pcm + (keep ? (size_t)(first - axis) * source.channels : 0);
        assert(fwrite(pcm, sizeof(int16_t) * source.channels, keep, native) == keep); native_frames += keep;
        uint32_t consumed_total = 0;
        while (consumed_total < keep) {
            int16_t converted[7]; uint32_t consumed, produced, input_frames = 1u + count % 17u;
            if (input_frames > keep - consumed_total) input_frames = keep - consumed_total;
            assert(!player_pcm_convert_process(&converter, pcm + (size_t)consumed_total * source.channels,
                        input_frames, &consumed, converted, 7, &produced) && (consumed || produced));
            assert(fwrite(converted, 2, produced, mono) == produced); consumed_total += consumed; output_frames += produced;
        }
        axis = end;
    }
    if (r == PLAYER_MEDIA_EOF) {
        int done = 0;
        while (!done) {
            int16_t converted[7]; uint32_t produced;
            done = player_pcm_convert_finish(&converter, converted, 7, &produced); assert(done >= 0);
            assert(fwrite(converted, 2, produced, mono) == produced); output_frames += produced;
        }
        uint64_t expected; assert(!player_pcm_convert_output_count(native_frames, source.source_rate, &expected));
        assert(expected == output_frames && player_source_next(&source, &p) == PLAYER_MEDIA_EOF);
    }
    assert(!fseek(bundle, 0, SEEK_SET)); le32(bundle, count);
    assert(!fclose(bundle) && !fclose(native) && !fclose(mono) && !fclose(video));
    free(packet); player_audio_decoder_destroy(decoder); player_source_close(&source); free(bytes);
    if (r != PLAYER_MEDIA_EOF) { printf("{\"error\":%d,\"stage\":\"packet/decode\"}\n", r); return 4; }
    printf("{\"done\":{\"packets\":%u,\"video\":%u,\"native_frames\":%" PRIu64 ",\"output_frames\":%" PRIu64 ",\"decoder_skipped_frames\":%u}}\n",
           count, video_count, native_frames, output_frames, skipped);
    return 0;
}
