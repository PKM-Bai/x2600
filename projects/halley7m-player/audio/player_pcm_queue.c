#include "player_pcm_queue.h"
#include <stddef.h>
#include <string.h>

_Static_assert(PLAYER_PCM_PERIOD_SAMPLES % PLAYER_PCM_DMA_SAMPLES == 0u,
               "full periods must contain complete DMA bursts");
_Static_assert(PLAYER_PCM_PRIME_PERIODS <= PLAYER_PCM_PERIOD_COUNT,
               "prefill must fit the queue");
_Static_assert(sizeof(player_pcm_period) % 64u == 0u,
               "each period must start on a cache line");

void player_pcm_reset(player_pcm_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
    queue->active_slot = -1;
}

static void seal_period(player_pcm_queue *queue, player_pcm_period *period)
{
    unsigned dma = (period->valid_samples + PLAYER_PCM_DMA_SAMPLES - 1u) &
                   ~(PLAYER_PCM_DMA_SAMPLES - 1u);
    /* The first 16 samples are put directly in the AIC FIFO. Always leave
     * at least one 32-byte DMA burst, including a stream shorter than 16. */
    if (dma < PLAYER_PCM_DMA_SAMPLES * 2u)
        dma = PLAYER_PCM_DMA_SAMPLES * 2u;
    memset(period->samples + period->valid_samples, 0,
           (dma - period->valid_samples) * sizeof(period->samples[0]));
    period->dma_samples = (uint16_t)dma;
    period->state = PLAYER_PCM_READY;
    ++queue->ready_count;
    queue->write_slot = (queue->write_slot + 1u) % PLAYER_PCM_PERIOD_COUNT;
}

unsigned player_pcm_write(player_pcm_queue *queue, const int16_t *pcm, unsigned samples)
{
    unsigned copied = 0;
    if (queue->eof || (!pcm && samples))
        return 0;
    while (copied < samples) {
        player_pcm_period *period = &queue->period[queue->write_slot];
        unsigned count;
        if (period->state == PLAYER_PCM_FREE) {
            period->valid_samples = 0;
            period->dma_samples = 0;
            period->state = PLAYER_PCM_FILLING;
        }
        if (period->state != PLAYER_PCM_FILLING)
            break;
        count = PLAYER_PCM_PERIOD_SAMPLES - period->valid_samples;
        if (count > samples - copied)
            count = samples - copied;
        memcpy(period->samples + period->valid_samples, pcm + copied,
               count * sizeof(*pcm));
        period->valid_samples = (uint16_t)(period->valid_samples + count);
        queue->submitted_samples += count;
        copied += count;
        if (period->valid_samples == PLAYER_PCM_PERIOD_SAMPLES)
            seal_period(queue, period);
    }
    return copied;
}

void player_pcm_finish(player_pcm_queue *queue)
{
    player_pcm_period *period = &queue->period[queue->write_slot];
    if (!queue->eof && period->state == PLAYER_PCM_FILLING && period->valid_samples)
        seal_period(queue, period);
    queue->eof = 1;
}

player_pcm_period *player_pcm_take(player_pcm_queue *queue)
{
    player_pcm_period *period;
    if (queue->active_slot >= 0 || !queue->ready_count)
        return NULL;
    period = &queue->period[queue->read_slot];
    if (period->state != PLAYER_PCM_READY)
        return NULL;
    period->state = PLAYER_PCM_ACTIVE;
    queue->active_slot = (int)queue->read_slot;
    queue->read_slot = (queue->read_slot + 1u) % PLAYER_PCM_PERIOD_COUNT;
    --queue->ready_count;
    return period;
}

int player_pcm_complete(player_pcm_queue *queue)
{
    player_pcm_period *period;
    if (queue->active_slot < 0)
        return -1;
    period = &queue->period[queue->active_slot];
    if (period->state != PLAYER_PCM_ACTIVE)
        return -1;
    queue->completed_samples += period->valid_samples;
    period->state = PLAYER_PCM_FREE;
    period->valid_samples = 0;
    period->dma_samples = 0;
    queue->active_slot = -1;
    return 0;
}

int player_pcm_drained(const player_pcm_queue *queue)
{
    return queue->eof && queue->active_slot < 0 && !queue->ready_count;
}
