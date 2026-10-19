#ifndef PLAYER_AUDIO_H
#define PLAYER_AUDIO_H

#include <stdint.h>
#include "player_pcm_queue.h"

enum player_audio_state {
    PLAYER_AUDIO_CLOSED, PLAYER_AUDIO_IDLE, PLAYER_AUDIO_BUFFERING,
    PLAYER_AUDIO_PLAYING, PLAYER_AUDIO_PAUSING, PLAYER_AUDIO_PAUSED,
    PLAYER_AUDIO_DRAINING, PLAYER_AUDIO_DRAINED, PLAYER_AUDIO_FAULT
};
enum player_audio_result {
    PLAYER_AUDIO_OK = 0, PLAYER_AUDIO_ARGUMENT = -1,
    PLAYER_AUDIO_STATE = -2, PLAYER_AUDIO_HARDWARE = -3,
    PLAYER_AUDIO_TIMEOUT = -4
};
typedef struct player_audio_stats {
    enum player_audio_state state;
    uint32_t submitted_samples;
    /* Samples handed to the FIFO, not a sub-period playback clock. */
    uint32_t completed_samples;
    uint32_t completed_periods;
    uint32_t queue_underruns;
    uint32_t fifo_underruns;
    uint32_t dma_errors;
    uint32_t dma_timeouts;
    unsigned ready_periods;
    unsigned volume_percent;
    int active_period;
    int last_error;
} player_audio_stats;

/* Only the selected AudioOwner links/initializes this hardware library.
 * One task owns all calls; no pointer crosses the AMP boundary.
 * open/close alter CPM shared registers: the caller must obtain the external
 * MIPS/RV clock handshake before invoking them. In the formal RV-owned build
 * MIPS never initializes PDMA. The MIPS-owned comparison keeps RV in reset.
 * open requires a running scheduler. begin includes the codec's 300 ms analog
 * settling delay. No task is created; service must be called at least every
 * 10 ms while a session is active, including while waiting for PCM space.
 */
int player_audio_open(void);
int player_audio_begin(void);
/* Copy PCM16LE/48kHz/mono, returning accepted samples (0 = backpressure).
 * Inspect stats.last_error/state for a hardware failure. */
unsigned player_audio_write(const int16_t *pcm, unsigned samples);
int player_audio_finish(void);
/* PAUSE takes effect after the current 10 ms period and FIFO drain. Ready
 * periods and EOF survive. Wait for PAUSED using service/stats before ACK. */
int player_audio_pause(void);
int player_audio_resume(void);
int player_audio_service(void);
/* STOP is immediate; queued PCM is discarded only after CTE reads disabled.
 * On failure private storage remains pinned and begin refuses to reuse it. */
int player_audio_stop(void);
int player_audio_close(void);
int player_audio_set_volume(unsigned percent);
void player_audio_get_stats(player_audio_stats *stats);

#endif
