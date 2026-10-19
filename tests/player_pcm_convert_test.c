#include "player_pcm_convert.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int16_t mono(const int16_t *pcm, uint32_t index, uint32_t channels)
{
    int32_t value = pcm[(size_t)index * channels];
    if (channels == 2u) value = (value + pcm[(size_t)index * 2u + 1u]) / 2;
    return (int16_t)value;
}

static void check(uint32_t rate, uint32_t channels, uint32_t count, unsigned chunk_mode)
{
    uint64_t expected;
    assert(!player_pcm_convert_output_count(count, rate, &expected));
    int16_t *pcm = malloc(((size_t)count * channels + 1u) * sizeof(*pcm));
    int16_t *output = malloc(((size_t)expected + 1u) * sizeof(*output));
    assert(pcm && output);
    for (uint32_t i = 0; i < count * channels; ++i)
        pcm[i] = (int16_t)((i * 1271u + (i % 7u) * 47u) & 65535u);
    player_pcm_converter c;
    assert(!player_pcm_convert_init(&c, rate, channels));
    uint32_t read = 0, written = 0, step = 0;
    while (read < count) {
        uint32_t block = chunk_mode ? 1u + (step * 73u % 177u) : count - read;
        uint32_t capacity = chunk_mode ? 1u + (step * 19u % 121u) : (uint32_t)expected + 1u;
        if (block > count - read) block = count - read;
        uint32_t consumed, produced;
        assert(!player_pcm_convert_process(&c, pcm + (size_t)read * channels, block, &consumed,
                                           output + written, capacity, &produced));
        assert(consumed || produced);
        read += consumed;
        written += produced;
        assert(written <= expected);
        ++step;
    }
    int done = 0;
    while (!done) {
        uint32_t produced;
        done = player_pcm_convert_finish(&c, output + written, chunk_mode ? 1u : (uint32_t)expected + 1u,
                                         &produced);
        assert(done >= 0);
        written += produced;
    }
    assert(written == expected && c.output_frames == expected && c.input_frames == count);
    for (uint32_t k = 0; k < written; ++k) {
        uint64_t numerator = (uint64_t)k * rate;
        uint32_t index = (uint32_t)(numerator / 48000u), phase = (uint32_t)(numerator % 48000u);
        int16_t a = mono(pcm, index < count ? index : count - 1u, channels);
        int16_t b = mono(pcm, index + 1u < count ? index + 1u : count - 1u, channels);
        int16_t reference = (int16_t)(a + ((int64_t)((int32_t)b - a) * phase) / 48000);
        assert(output[k] == reference);
    }
    uint32_t n;
    assert(player_pcm_convert_finish(&c, output, 1, &n) == 1 && !n);
    assert(player_pcm_convert_process(&c, pcm, 1, &n, output, 1, &n) == -EINVAL);
    free(output);
    free(pcm);
}

int main(void)
{
    const uint32_t rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    const uint32_t counts[] = {0, 1, 2, 17, 1024, 88237};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r)
        for (unsigned c = 1; c <= 2; ++c)
            for (unsigned n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n)
                for (unsigned chunks = 0; chunks < 2; ++chunks) check(rates[r], c, counts[n], chunks);
    uint64_t total;
    assert(!player_pcm_convert_output_count(UINT64_C(44100) * 60u * 60u * 24u, 44100, &total));
    assert(total == UINT64_C(48000) * 60u * 60u * 24u);
    assert(player_pcm_convert_output_count(UINT64_MAX, 8000, &total) == -EOVERFLOW);
    assert(player_pcm_convert_output_count(10, 96000, &total) == -EINVAL);
    puts("PASS PCM converter: real integer interpolation, 9 rates mono/stereo, arbitrary chunks/backpressure, exact rational EOF/empty/tail, 24h count no drift");
    return 0;
}
