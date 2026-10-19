#include "player_pcm_convert.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

static int rate_valid(uint32_t rate) { return rate >= 8000u && rate <= 48000u; }

int player_pcm_convert_output_count(uint64_t frames, uint32_t rate, uint64_t *output)
{
    if (!output || !rate_valid(rate)) return -EINVAL;
    uint64_t whole = frames / rate, remainder = frames % rate;
    uint64_t tail = remainder * UINT64_C(48000) / rate;
    if (whole > (UINT64_MAX - tail) / UINT64_C(48000)) return -EOVERFLOW;
    *output = whole * UINT64_C(48000) + tail;
    return 0;
}

int player_pcm_convert_init(player_pcm_converter *c, uint32_t rate, uint32_t channels)
{
    if (!c || !rate_valid(rate) || (channels != 1u && channels != 2u)) return -EINVAL;
    memset(c, 0, sizeof(*c));
    c->sample_rate = rate;
    c->channels = channels;
    return 0;
}

static void advance_output(player_pcm_converter *c)
{
    uint32_t phase = c->phase + c->sample_rate;
    c->source_index += phase / 48000u;
    c->phase = phase % 48000u;
    ++c->output_frames;
}

static int can_output(const player_pcm_converter *c)
{
    return c->input_frames && (c->source_index < c->input_frames - 1u ||
           (c->source_index == c->input_frames - 1u && !c->phase));
}

int player_pcm_convert_process(player_pcm_converter *c, const int16_t *input,
                               uint32_t frames, uint32_t *consumed, int16_t *output,
                               uint32_t capacity, uint32_t *produced)
{
    if (consumed) *consumed = 0;
    if (produced) *produced = 0;
    if (!c || !consumed || !produced || !rate_valid(c->sample_rate) ||
        (c->channels != 1u && c->channels != 2u) || c->finishing ||
        (frames && !input) || (capacity && !output)) return -EINVAL;
    while (*produced < capacity) {
        if (can_output(c)) {
            int32_t value = c->current;
            if (c->source_index < c->input_frames - 1u) {
                int32_t difference = (int32_t)c->current - c->previous;
                value = c->previous + (int32_t)((int64_t)difference * c->phase / 48000);
            }
            output[(*produced)++] = (int16_t)value;
            advance_output(c);
        } else if (*consumed < frames) {
            if (c->input_frames == UINT64_MAX) return -EOVERFLOW;
            size_t index = (size_t)*consumed * c->channels;
            int32_t value = input[index];
            if (c->channels == 2u) value = (value + input[index + 1u]) / 2;
            c->previous = c->current;
            c->current = (int16_t)value;
            ++c->input_frames;
            ++*consumed;
        } else break;
    }
    return 0;
}

int player_pcm_convert_finish(player_pcm_converter *c, int16_t *output,
                              uint32_t capacity, uint32_t *produced)
{
    if (produced) *produced = 0;
    if (!c || !produced || !rate_valid(c->sample_rate) ||
        (c->channels != 1u && c->channels != 2u) || (capacity && !output)) return -EINVAL;
    c->finishing = 1;
    uint64_t total;
    int result = player_pcm_convert_output_count(c->input_frames, c->sample_rate, &total);
    if (result) return result;
    while (c->output_frames < total && *produced < capacity) {
        int32_t value = c->current;
        if (c->source_index < c->input_frames - 1u) {
            int32_t difference = (int32_t)c->current - c->previous;
            value = c->previous + (int32_t)((int64_t)difference * c->phase / 48000);
        }
        output[(*produced)++] = (int16_t)value;
        advance_output(c);
    }
    c->finished = c->output_frames == total;
    return c->finished ? 1 : 0;
}
