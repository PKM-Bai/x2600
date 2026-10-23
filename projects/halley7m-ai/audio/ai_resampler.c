#include "ai_resampler.h"
#include <limits.h>
#include <string.h>

/* 48 taps per fractional phase; cutoff 7000 Hz at 24000 Hz.
 * q[j] = normalized sinc(2*fc*(j-23-phase/2)) * Blackman(j/47), Q30.
 * The integer coefficient sum of each phase is exactly 1<<30.
 * Generated formula is recorded in README; no floating point at runtime. */
static const int32_t coefficients[2][48] = {
    {
        0, 12597, 76066, -226707, -131747, 877818, -363508, -1847927,
        2205906, 2212982, -5919964, 0, 10730615, -7320531, -13509768, 21454870,
        8297098, -41631534, 14158800, 63826970, -73742488, -81760044, 325327408, 626346820,
        330135317, -84200929, -77084109, 67736800, 15260186, -45587837, 9235743, 24292302,
        -15571403, -8597564, 12856527, 0, -7417629, 2845513, 2920987, -2532818,
        -519782, 1326462, -215238, -418791, 176291, 51342, -23278, 0
    },
    {
        0, -9410, 103982, -33274, -457384, 518986, 829047, -1908133,
        -393335, 4229018, -2254382, -6324474, 8414832, 5326305, -17912386, 3037855,
        27592127, -23351270, -30219533, 60414265, 11646267, -129459148, 85768606, 541312351,
        541312351, 85768606, -129459148, 11646267, 60414265, -30219533, -23351270, 27592127,
        3037855, -17912386, 5326305, 8414832, -6324474, -2254382, 4229018, -393335,
        -1908133, 829047, 518986, -457384, -33274, 103982, -9410, 0
    }
};

void ai_resampler_init(ai_resampler *r)
{
    if (r) memset(r, 0, sizeof(*r));
}

uint64_t ai_resampler_output_count(uint64_t input_samples)
{
    return (input_samples / 3) * 2 + (input_samples % 3) * 2 / 3;
}

static uint64_t source_index(const ai_resampler *r)
{
    return (r->output_samples / 2) * 3 + (r->output_samples % 2);
}

static int16_t output_sample(ai_resampler *r)
{
    uint64_t source = source_index(r);
    unsigned phase = (unsigned)(r->output_samples & 1u);
    int64_t sum = 0;
    for (unsigned j = 0; j < 48; ++j) {
        int offset = (int)j - 23;
        if (offset < 0 && source < (uint64_t)-offset) continue;
        uint64_t index = offset < 0 ? source - (uint64_t)-offset :
                                     source + (uint64_t)offset;
        if (index < r->input_samples)
            sum += (int64_t)r->history[index % AI_RESAMPLER_HISTORY] *
                   coefficients[phase][j];
    }
    /* Division is defined for negatives, unlike signed right shift. */
    sum = sum >= 0 ? (sum + INT64_C(536870912)) / INT64_C(1073741824) :
                    -((-sum + INT64_C(536870912)) / INT64_C(1073741824));
    if (sum > INT16_MAX) sum = INT16_MAX;
    if (sum < INT16_MIN) sum = INT16_MIN;
    ++r->output_samples;
    return (int16_t)sum;
}

int ai_resampler_process(ai_resampler *r, const int16_t *input, size_t count,
                         size_t *consumed, int16_t *output, size_t capacity,
                         size_t *produced)
{
    if (consumed) *consumed = 0;
    if (produced) *produced = 0;
    if (!r || !consumed || !produced || (count && !input) ||
        (capacity && !output) || r->finishing) return -1;
    while (*produced < capacity) {
        if (r->input_samples > source_index(r) + 24) {
            output[(*produced)++] = output_sample(r);
        } else if (*consumed < count) {
            /* Any practical utterance is far below this arithmetic bound. */
            if (r->input_samples >= UINT64_MAX - 64) return -1;
            r->history[r->input_samples % AI_RESAMPLER_HISTORY] = input[(*consumed)++];
            ++r->input_samples;
        } else break;
    }
    return 0;
}

int ai_resampler_finish(ai_resampler *r, int16_t *output, size_t capacity,
                        size_t *produced)
{
    if (produced) *produced = 0;
    if (!r || !produced || (capacity && !output)) return -1;
    r->finishing = 1;
    uint64_t target = ai_resampler_output_count(r->input_samples);
    while (*produced < capacity && r->output_samples < target)
        output[(*produced)++] = output_sample(r);
    return r->output_samples == target ? 1 : 0;
}
