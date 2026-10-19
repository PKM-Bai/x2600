#include "player_pcm_queue.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static int ordered_and_backpressure(void)
{
    player_pcm_queue queue;
    int16_t input[PLAYER_PCM_PERIOD_SAMPLES * (PLAYER_PCM_PERIOD_COUNT + 1u)];
    unsigned i;
    unsigned cursor = 0;
    player_pcm_reset(&queue);
    for (i = 0; i < sizeof(input) / sizeof(input[0]); ++i)
        input[i] = (int16_t)(i + 7u);
    CHECK(((uintptr_t)&queue.period[0].samples[0] & 63u) == 0u);
    CHECK(((uintptr_t)&queue.period[1].samples[0] & 63u) == 0u);
    CHECK(player_pcm_write(&queue, input, (unsigned)(sizeof(input) / sizeof(input[0]))) == 2880u);
    CHECK(queue.ready_count == PLAYER_PCM_PERIOD_COUNT);
    CHECK(player_pcm_write(&queue, input, 1) == 0);
    for (i = 0; i < PLAYER_PCM_PERIOD_COUNT; ++i) {
        player_pcm_period *period = player_pcm_take(&queue);
        CHECK(period != NULL);
        CHECK(player_pcm_take(&queue) == NULL);
        CHECK(period->valid_samples == PLAYER_PCM_PERIOD_SAMPLES);
        CHECK(period->dma_samples == PLAYER_PCM_PERIOD_SAMPLES);
        CHECK(!memcmp(period->samples, input + cursor, sizeof(period->samples)));
        cursor += PLAYER_PCM_PERIOD_SAMPLES;
        CHECK(player_pcm_complete(&queue) == 0);
    }
    CHECK(queue.completed_samples == 2880u);
    CHECK(player_pcm_complete(&queue) < 0);
    CHECK(player_pcm_take(&queue) == NULL);
    /* Wrap both producer and consumer without replaying an old slot. */
    CHECK(player_pcm_write(&queue, input + 2880u, 480u) == 480u);
    player_pcm_finish(&queue);
    CHECK(player_pcm_write(&queue, input, 1u) == 0);
    CHECK(!player_pcm_drained(&queue));
    CHECK(!memcmp(player_pcm_take(&queue)->samples, input + 2880u, 960u));
    CHECK(!player_pcm_drained(&queue));
    CHECK(player_pcm_complete(&queue) == 0);
    CHECK(player_pcm_drained(&queue));
    CHECK(queue.submitted_samples == 3360u);
    CHECK(queue.completed_samples == 3360u);
    return 0;
}

static int every_tail_length(void)
{
    player_pcm_queue queue;
    int16_t input[PLAYER_PCM_PERIOD_SAMPLES];
    unsigned count;
    unsigned i;
    for (i = 0; i < PLAYER_PCM_PERIOD_SAMPLES; ++i)
        input[i] = (int16_t)(i - 240);
    for (count = 1; count <= PLAYER_PCM_PERIOD_SAMPLES; ++count) {
        player_pcm_period *period;
        player_pcm_reset(&queue);
        /* Seed stale data to detect a final burst replaying old contents. */
        memset(queue.period[0].samples, 0x55, sizeof(queue.period[0].samples));
        CHECK(player_pcm_write(&queue, input, count / 2u) == count / 2u);
        CHECK(player_pcm_write(&queue, input + count / 2u, count - count / 2u) == count - count / 2u);
        if (count < PLAYER_PCM_PERIOD_SAMPLES)
            CHECK(player_pcm_take(&queue) == NULL);
        player_pcm_finish(&queue);
        player_pcm_finish(&queue);
        CHECK(queue.ready_count == 1u);
        period = player_pcm_take(&queue);
        CHECK(period != NULL);
        CHECK(period->valid_samples == count);
        CHECK(period->dma_samples >= 32u);
        CHECK(period->dma_samples >= count);
        CHECK(period->dma_samples % 16u == 0u);
        CHECK(period->dma_samples <= 480u);
        CHECK(!memcmp(period->samples, input, count * sizeof(input[0])));
        for (i = count; i < period->dma_samples; ++i)
            CHECK(period->samples[i] == 0);
        CHECK(player_pcm_complete(&queue) == 0);
        CHECK(queue.completed_samples == count);
        CHECK(queue.submitted_samples == count);
        CHECK(player_pcm_drained(&queue));
    }
    return 0;
}

static int partial_survives_owner_pause_and_stop(void)
{
    player_pcm_queue queue;
    int16_t input[961];
    player_pcm_period *period;
    unsigned i;
    for (i = 0; i < 961u; ++i)
        input[i] = (int16_t)i;
    player_pcm_reset(&queue);
    CHECK(player_pcm_write(&queue, input, 961u) == 961u);
    CHECK(queue.ready_count == 2u);
    CHECK(player_pcm_take(&queue)->samples[0] == 0);
    CHECK(player_pcm_complete(&queue) == 0);
    /* PAUSE stops calling take: a successor and partial PCM remain private.
     * Producer may signal EOF while paused. Neither changes read order. */
    CHECK(queue.completed_samples == 480u);
    CHECK(queue.ready_count == 1u);
    player_pcm_finish(&queue);
    CHECK(queue.ready_count == 2u);
    CHECK(player_pcm_take(&queue)->samples[0] == 480);
    CHECK(player_pcm_complete(&queue) == 0);
    period = player_pcm_take(&queue);
    CHECK(period->valid_samples == 1u && period->dma_samples == 32u);
    CHECK(period->samples[0] == 960);
    CHECK(player_pcm_complete(&queue) == 0);
    CHECK(queue.completed_samples == 961u);
    CHECK(player_pcm_drained(&queue));
    player_pcm_reset(&queue);
    CHECK(player_pcm_write(&queue, input, 961u) == 961u);
    CHECK(player_pcm_take(&queue) != NULL);
    /* STOP resets only after the hardware adapter establishes quiescence. */
    player_pcm_reset(&queue);
    CHECK(queue.ready_count == 0u && queue.active_slot == -1);
    CHECK(queue.submitted_samples == 0u && queue.completed_samples == 0u);
    CHECK(!queue.eof);
    player_pcm_finish(&queue);
    CHECK(player_pcm_drained(&queue));
    CHECK(player_pcm_take(&queue) == NULL);
    return 0;
}

int main(void)
{
    if (ordered_and_backpressure() || every_tail_length() || partial_survives_owner_pause_and_stop())
        return 1;
    printf("player_pcm_queue: %u checks passed (all 1..480 sample tails)\n", checks);
    return 0;
}
