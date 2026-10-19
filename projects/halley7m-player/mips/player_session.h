#ifndef PLAYER_SESSION_WORKER_H
#define PLAYER_SESSION_WORKER_H

#include "../common/player_session.h"

/* Call both only from the single media/storage worker task. init does not
 * create tasks; step makes bounded progress and must run at least every 5 ms.
 * The GUI uses only the public common header. */
int player_session_init(void);
void player_session_step(void);

#define PLAYER_SESSION_TRACE_CAPACITY 512u
enum player_session_trace_event {
    PLAYER_SESSION_TRACE_STATE = 1, PLAYER_SESSION_TRACE_DECODE,
    PLAYER_SESSION_TRACE_SUBMIT, PLAYER_SESSION_TRACE_LATCH,
    PLAYER_SESSION_TRACE_DROP, PLAYER_SESSION_TRACE_WAIT_PTS,
    PLAYER_SESSION_TRACE_SUBMIT_BUSY, PLAYER_SESSION_TRACE_CODEC_BUSY
};
typedef struct player_session_trace {
    uint32_t sequence, session, event, frame;
    uint32_t time_us_low; /* Modulo32; LATCH uses actual IRQ time, others owner time. */
    uint32_t audio_samples; /* Last observed valid DMA sample clock. */
    uint32_t duration_us; /* DECODE duration or SUBMIT-to-LATCH delay. */
    uint32_t irq_sequence;
    uint32_t queues; /* video packet count low16, audio packet count high16. */
    uint32_t flags; /* bit0 video_pending, bit1 has_ready, bit2 audio_started, state<<8. */
    uint32_t holds; /* Four pool-slot hold nibbles, lowest nibble slot0. */
    int32_t result;
} player_session_trace_t;
/* Fixed RAM circular trace: no allocation or per-frame UART. New media clears
 * it automatically. Readers copy a small batch after sequence; an overwritten
 * cursor resumes at oldest retained record. latest is the newest sequence.
 * Both calls are safe against the one owner writer. Dump after playback. */
void player_session_trace_reset(void);
unsigned player_session_trace_read(uint32_t after_sequence, player_session_trace_t *records,
                                    unsigned capacity, uint32_t *latest);

#endif
