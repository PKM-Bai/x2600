/* Binary packet bundle peer; packet boundaries come from real ffprobe. */
#include "player_audio_decode.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t read32(FILE *file)
{
    uint8_t b[4];
    assert(fread(b, 1, 4, file) == 4);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int main(int argc, char **argv)
{
    player_audio_decoder *decoder = NULL;
    assert(player_audio_decoder_create(NULL, &decoder) == -EINVAL);
    player_audio_decoder_destroy(NULL);
    const uint8_t lc_stereo_48k[] = {0x11, 0x90};
    const uint8_t main_stereo_48k[] = {0x09, 0x90};
    const uint8_t lc_stereo_96k[] = {0x10, 0x10};
    const uint8_t lc_multichannel_48k[] = {0x11, 0xb0};
    const uint8_t lc_960_frame[] = {0x11, 0x94};
    player_audio_decode_config valid = {PLAYER_AUDIO_CODEC_AAC_RAW, lc_stereo_48k, 2};
    assert(!player_audio_decoder_create(&valid, &decoder));
    player_audio_decoder_destroy(decoder);
    const uint8_t *unsupported[] = {main_stereo_48k, lc_stereo_96k, lc_multichannel_48k, lc_960_frame};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        valid.audio_specific_config = unsupported[i];
        assert(player_audio_decoder_create(&valid, &decoder) == -ENOTSUP && !decoder);
    }
    if (argc != 5 && argc != 6) {
        puts("Usage: decoder_test <codec1/2/3> <packets> <output.s16> <stats.json> [asc]");
        return argc == 1 ? 0 : 2;
    }
    player_audio_decode_config config = {0};
    config.codec = (enum player_audio_codec)atoi(argv[1]);
    uint8_t asc[64];
    if (argc == 6) {
        FILE *file = fopen(argv[5], "rb");
        assert(file);
        config.audio_specific_config_bytes = (uint32_t)fread(asc, 1, sizeof(asc), file);
        config.audio_specific_config = asc;
        assert(!fclose(file));
    }
    int created = player_audio_decoder_create(&config, &decoder);
    if (created) { fprintf(stderr, "create=%d\n", created); return 3; }
    FILE *input = fopen(argv[2], "rb"), *output = fopen(argv[3], "wb");
    assert(input && output);
    uint32_t packets = read32(input), rate = 0, channels = 0, skipped = 0;
    uint64_t frames = 0;
    uint8_t *data = malloc(65536u);
    assert(data);
    for (uint32_t i = 0; i < packets; ++i) {
        uint32_t bytes = read32(input);
        assert(bytes && bytes <= 65536u && fread(data, 1, bytes, input) == bytes);
        player_audio_decoded_frame decoded;
        int result = player_audio_decoder_decode(decoder, data, bytes, &decoded);
        if (result) { fprintf(stderr, "decode packet=%u bytes=%u result=%d\n", i, bytes, result); return 4; }
        assert(decoded.consumed_bytes == bytes && decoded.pcm);
        assert(!rate || rate == decoded.sample_rate);
        assert(!channels || channels == decoded.channels);
        rate = decoded.sample_rate;
        channels = decoded.channels;
        frames += decoded.frames;
        skipped += decoded.decoder_skipped_frames;
        size_t samples = (size_t)decoded.frames * channels;
        assert(fwrite(decoded.pcm, sizeof(int16_t), samples, output) == samples);
    }
    assert(fgetc(input) == EOF && !fclose(input) && !fclose(output));
    free(data);
    player_audio_decoder_destroy(decoder);
    FILE *stats = fopen(argv[4], "wb");
    assert(stats);
    fprintf(stats, "{\"frames\":%llu,\"sample_rate\":%u,\"channels\":%u,\"decoder_skipped_frames\":%u,\"packets\":%u}\n",
            (unsigned long long)frames, rate, channels, skipped, packets);
    assert(!fclose(stats));
    return 0;
}
