#ifndef PLAYER_PCM_QUEUE_H
#define PLAYER_PCM_QUEUE_H

#include <stdint.h>

#define PLAYER_PCM_RATE 48000u
#define PLAYER_PCM_PERIOD_SAMPLES 480u
#define PLAYER_PCM_PERIOD_COUNT 6u
#define PLAYER_PCM_PRIME_PERIODS 5u
#define PLAYER_PCM_DMA_SAMPLES 16u

/* Private-memory queue. The owner serializes every call against its DMA ISR.
 * DMA sample counts include final zero padding; completed_samples never does.
 */
enum player_pcm_slot_state { PLAYER_PCM_FREE, PLAYER_PCM_FILLING,
                             PLAYER_PCM_READY, PLAYER_PCM_ACTIVE };
typedef struct player_pcm_period {
    _Alignas(64) int16_t samples[PLAYER_PCM_PERIOD_SAMPLES];
    uint16_t valid_samples;
    uint16_t dma_samples;
    enum player_pcm_slot_state state;
} player_pcm_period;

typedef struct player_pcm_queue {
    player_pcm_period period[PLAYER_PCM_PERIOD_COUNT];
    uint32_t submitted_samples;
    uint32_t completed_samples;
    unsigned read_slot;
    unsigned write_slot;
    unsigned ready_count;
    int active_slot;
    unsigned eof;
} player_pcm_queue;

void player_pcm_reset(player_pcm_queue *queue);
/* Copies and returns the number accepted. A full queue applies backpressure. */
unsigned player_pcm_write(player_pcm_queue *queue, const int16_t *pcm, unsigned samples);
/* Seal the partial period, if any; no more writes until reset. Idempotent. */
void player_pcm_finish(player_pcm_queue *queue);
/* Returns NULL if no ready period or a previous period is still active. */
player_pcm_period *player_pcm_take(player_pcm_queue *queue);
/* Called only after successful DMA termination, never on STOP/error. */
int player_pcm_complete(player_pcm_queue *queue);
int player_pcm_drained(const player_pcm_queue *queue);

#endif
