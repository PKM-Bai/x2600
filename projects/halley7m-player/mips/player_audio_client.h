#ifndef PLAYER_AUDIO_CLIENT_H
#define PLAYER_AUDIO_CLIENT_H

#include "player_amp.h"
#include <stdint.h>

typedef struct player_audio_client_stats {
    uint32_t media_session_id;
    uint32_t total_samples;
    uint32_t written_samples;
    /* Valid samples completed by DMA into the FIFO, at period granularity;
     * final zero padding is excluded. PAUSED/ENDED additionally wait for drain.
     */
    uint32_t completed_samples;
    uint32_t underruns;
    uint32_t dma_errors;
    uint32_t dma_timeouts;
    uint32_t queue_underruns;
    uint32_t fifo_underruns;
    uint32_t volume_percent;
    uint32_t state; /* enum player_amp_audio_state */
    uint32_t clock_valid;
    uint32_t session_active;
    int32_t last_error;
} player_audio_client_stats;

enum player_audio_client_result {
    PLAYER_AUDIO_CLIENT_PROTOCOL = -10,
    PLAYER_AUDIO_CLIENT_UNAVAILABLE = -11,
    PLAYER_AUDIO_CLIENT_TIMEOUT = -12,
    PLAYER_AUDIO_CLIENT_BUSY = -13,
    PLAYER_AUDIO_CLIENT_ARGUMENT = -14,
    PLAYER_AUDIO_CLIENT_STATE = -15
};

/* One MIPS media-control task owns every call, for both AudioOwner builds.
 * Format is 48000 Hz mono signed PCM16LE; counts refer to samples, not bytes.
 * Initialize storage/video clocks BEFORE start. STOP success confirms mute,
 * DMA quiescence and audio clock release; only then tear down display clocks
 * or begin another session. START never boots/resets the RISC-V implicitly.
 * Lost/malformed replies force PE17 low and hold RV reset without touching
 * its DMA/codec. PCM/private periods remain pinned until a whole-board reset;
 * these failures never count as a successful STOP or permit another session.
 */
int player_audio_client_start(uint32_t total_samples, unsigned volume_percent);
/* Copies accepted samples. Zero means backpressure or error; inspect status.
 * Call service/status at least every 5 ms while waiting for PCM space or DMA.
 */
unsigned player_audio_client_write_pcm(const int16_t *samples, unsigned count);
int player_audio_client_mark_eof(void);
int player_audio_client_pause(void);
int player_audio_client_resume(void);
int player_audio_client_set_volume(unsigned percent);
int player_audio_client_service(void);
void player_audio_client_get_status(player_audio_client_stats *status);
int player_audio_client_stop(void);

#endif
