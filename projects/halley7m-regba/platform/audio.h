#ifndef HALLEY7M_REGBA_AUDIO_H
#define HALLEY7M_REGBA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_OUTPUT_RATE 48000u
#define AUDIO_PERIOD_FRAMES 480u
#define AUDIO_PERIOD_COUNT 6u
/* One more complete 10ms period than the previous four-period prefill. The
 * six-period allocation stays unchanged; producer stall attribution is separate.
 */
#define AUDIO_PRIME_PERIODS 5u
/* One 32-byte DMA burst of real mono S16 PCM, before enabling replay. */
#define AUDIO_FIFO_PREFILL_FRAMES 16u

typedef struct {
    uint32_t input_rate;
    uint32_t input_frames;
    uint32_t output_frames;
    uint32_t completed_periods;
    uint32_t queue_underruns;
    uint32_t fifo_underruns;
    uint32_t dma_errors;
    uint32_t dma_timeouts;
    uint32_t queued_frames;
    uint32_t running;
    uint32_t muted;
    uint32_t volume_percent;
    uint32_t input_nonzero_frames;
    uint32_t input_peak;
    /* Software observations: full-period publication and IRQ-to-rearm.
     * Neither is a hardware measurement of the precise FIFO empty instant.
     */
    uint32_t max_ready_gap_us;
    uint32_t underrun_ready_gap_us;
    uint32_t max_dma_restart_us;
    uint64_t first_queue_underrun_us;
    uint32_t last_ready_periods;
    uint32_t last_ready_active;
    uint32_t last_ready_dma_age_us;
    /* The last-publication snapshot copied at the first queue underrun.
     * Active DMA age is software elapsed time, not exact samples remaining.
     */
    uint32_t first_under_ready_periods;
    uint32_t first_under_active;
    uint32_t first_under_dma_age_us;
    uint64_t first_fifo_underrun_us;
    uint32_t first_fifo_completed_periods;
} audio_stats_t;

/* Call init/start/submit from one producer task after the scheduler starts.
 * Init creates the high-priority DMA service task but does not play sound.
 * Start resets the ring, converter and statistics; stop discards queued PCM.
 * This backend exclusively owns the main PDMA controller in the v1 build.
 */
int audio_init(void);
int audio_start(void);
void audio_stop(void);
/* May be called by the console task: cancels a diagnostic tone and cuts off
 * DMA and the amplifier immediately. A running tone returns -ECANCELED.
 */
void audio_request_stop(void);
/* Keep conversion and DMA active while muting the DAC and disabling PE17.
 * May be changed from the console; preserved across stop/start.
 */
void audio_set_muted(int muted);
/* Runtime volume, 0..100 percent; default 100 preserves the original DAC gain.
 * Zero mutes the DAC and disables the amplifier without changing explicit
 * mute or discarding queued PCM. May be changed from any task, before init;
 * retained across stop/start and ROM switches. Out of range returns -EINVAL.
 */
int audio_set_volume(unsigned percent);
unsigned audio_get_volume(void);

/* Interleaved signed-16 L/R, input rate in Hz (8000..192000).
 * Nonblocking: returns input frames consumed; retain and retry any remainder.
 * Keep the input rate fixed until stop/start. A rate change returns zero.
 * Linear conversion retains its last source sample across calls. Output is
 * averaged to mono using a 32-bit sum. Partial DMA periods remain queued until
 * full; stop intentionally discards the final partial period.
 */
size_t audio_submit_stereo(const int16_t *samples, size_t frames,
                           uint32_t sample_rate);
void audio_get_stats(audio_stats_t *stats);

/* Explicit diagnostic only: 500 Hz, about -30 dBFS, fixed two-second sine
 * with 50 ms fades; blocks caller and returns -ECANCELED when interrupted.
 * Uses the same ring and DMA path. Never invoked automatically by init.
 * Arm at command acceptance so a stop while queued is retained by the worker.
 */
void audio_prepare_test_tone(void);
int audio_test_tone(void);

#endif
