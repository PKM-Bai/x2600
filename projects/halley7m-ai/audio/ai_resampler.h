#ifndef AI_RESAMPLER_H
#define AI_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

#define AI_RESAMPLER_HISTORY 64u

/* Fixed 24000 -> 16000 Hz signed mono PCM. Host int16_t samples, no allocation.
 * A 48-tap fractional-phase low-pass FIR uses 24 input samples (1 ms) lookahead.
 * Process/finish retain fractional phase across arbitrary block boundaries.
 * Call finish to zero-pad the filter edge without losing the utterance tail;
 * total output is exactly floor(input_samples * 2 / 3), including short inputs.
 * Reset before every new/cancelled response so filter history cannot leak. */
typedef struct {
    int16_t history[AI_RESAMPLER_HISTORY];
    uint64_t input_samples, output_samples;
    int finishing;
} ai_resampler;

void ai_resampler_init(ai_resampler *resampler);
uint64_t ai_resampler_output_count(uint64_t input_samples);
/* Both counts are sample counts. 0 success, -1 invalid arguments/state.
 * With zero output capacity nothing is consumed. Drain remaining input until
 * consumed==count; capacity exhaustion is normal backpressure. */
int ai_resampler_process(ai_resampler *resampler, const int16_t *input,
                         size_t count, size_t *consumed, int16_t *output,
                         size_t capacity, size_t *produced);
/* Returns 1 when complete, 0 when more output capacity is needed, -1 invalid.
 * Repeated finish after completion returns 1/produced=0; process is then invalid. */
int ai_resampler_finish(ai_resampler *resampler, int16_t *output, size_t capacity,
                        size_t *produced);

#endif
