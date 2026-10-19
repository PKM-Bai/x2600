#ifndef PLAYER_PCM_CONVERT_H
#define PLAYER_PCM_CONVERT_H

#include <stdint.h>

typedef struct player_pcm_converter {
    uint64_t input_frames, output_frames, source_index;
    uint32_t sample_rate, channels, phase;
    int16_t previous, current;
    uint32_t finishing, finished;
} player_pcm_converter;

/* Stateless count rule: floor(source_frames*48000/source_rate), checked for
 * overflow. Call after any container/encoder source-sample trims. */
int player_pcm_convert_output_count(uint64_t source_frames, uint32_t source_rate,
                                    uint64_t *output_frames);
/* Rates 8000..48000, channels1/2. Stereo -> mono arithmetic average, then
 * deterministic linear interpolation at rational k*source_rate/48000.
 * Source format remains fixed per session; reset with init for a new file. */
int player_pcm_convert_init(player_pcm_converter *converter, uint32_t source_rate,
                            uint32_t channels);
/* Inputs/consumed count source PCM frames. Output is S16 mono at 48000Hz.
 * May consume only a prefix under backpressure; retain unconsumed input.
 * Neither input nor output is retained by pointer. Outputs are identical for
 * every input/output chunk partition. 0=success, negative errno=invalid call. */
int player_pcm_convert_process(player_pcm_converter *converter, const int16_t *input,
                               uint32_t input_frames, uint32_t *consumed,
                               int16_t *output, uint32_t output_capacity,
                               uint32_t *produced);
/* EOF holds the final source sample to reach exactly output_count(). Call
 * repeatedly when output capacity is small. 1=finished, 0=more tail pending;
 * negative errno=error. Repeated completed finish emits no samples. */
int player_pcm_convert_finish(player_pcm_converter *converter, int16_t *output,
                              uint32_t output_capacity, uint32_t *produced);

#endif
