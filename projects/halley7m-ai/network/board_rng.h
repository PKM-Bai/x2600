#ifndef AI_BOARD_RNG_H
#define AI_BOARD_RNG_H

#include <stdint.h>

typedef struct {
    uint32_t configuration;
    uint32_t samples;
    uint32_t repeated_neighbors;
    uint32_t zero_words;
    uint32_t all_one_words;
    uint32_t one_bits;
    uint32_t varying_bits;
    uint32_t elapsed_us;
} ai_rng_probe_t;

/* Explicit diagnostic only, before any RNG consumer is running. Restores
 * original configuration/gate. No generated words are returned as TLS entropy.
 * Returns 0, -EINVAL, -EBUSY, -ETIMEDOUT, or -EIO for constant output. */
int ai_board_rng_probe(unsigned sample_count, ai_rng_probe_t *report);

#endif
