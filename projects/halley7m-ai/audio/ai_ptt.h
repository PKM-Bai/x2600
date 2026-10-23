#ifndef AI_PTT_H
#define AI_PTT_H

#include <stdbool.h>
#include <stdint.h>

#define AI_PTT_MAX_MS 30000u
#define AI_PTT_MAX_SAMPLES 480000u

typedef enum {
    AI_PTT_IDLE, AI_PTT_STOPPING, AI_PTT_RECORDING, AI_PTT_WAITING, AI_PTT_PLAYING
} ai_ptt_state;

enum {
    AI_PTT_START_CAPTURE = 1u << 0,
    AI_PTT_STOP_CAPTURE = 1u << 1,
    AI_PTT_SUBMIT_CAPTURE = 1u << 2,
    AI_PTT_DISCARD_CAPTURE = 1u << 3,
    AI_PTT_STOP_PLAYBACK = 1u << 4,
    AI_PTT_CANCEL_RESPONSE = 1u << 5,
    AI_PTT_NEED_STOP_TIMING = 1u << 6
};

/* Single task owns the core. Returned action bits describe work the adapter
 * must perform; no UART, network, allocation or clocks are accessed here. */
typedef struct {
    ai_ptt_state state;
    uint32_t generation, captured_samples, capture_started_ms;
    uint32_t stop_started_ms, tail_delay_ms;
    bool held, tail_delay_known, may_be_audible, stop_pending;
} ai_ptt;

void ai_ptt_init(ai_ptt *ptt);
/* Explicit measured/calibrated delay from the last old PCM sent to UART until
 * the speaker is quiet. There is deliberately no built-in verified default.
 * Set while idle or stopping. Returns 0, or -1 if busy/invalid. */
int ai_ptt_set_tail_delay(ai_ptt *ptt, uint32_t measured_ms);
uint32_t ai_ptt_press(ai_ptt *ptt, uint32_t now_ms);
uint32_t ai_ptt_release(ai_ptt *ptt, uint32_t now_ms);
uint32_t ai_ptt_tick(ai_ptt *ptt, uint32_t now_ms);
/* Call AFTER the adapter has stopped old UART output. Starts the measured
 * delay; until this acknowledgement, STOPPING can never start capture. */
void ai_ptt_playback_stopped(ai_ptt *ptt, uint32_t generation, uint32_t now_ms);
/* Accept only the returned sample count into the recording. Process returned
 * action bits after copying accepted samples. A full recording submits once. */
uint32_t ai_ptt_capture(ai_ptt *ptt, uint32_t samples, uint32_t *accepted);
bool ai_ptt_response_current(const ai_ptt *ptt, uint32_t generation);
bool ai_ptt_playback_begin(ai_ptt *ptt, uint32_t generation);
/* Called after the final old PCM has been handed to UART, not merely when the
 * cloud response ends. Tail waiting also applies after natural playback end. */
void ai_ptt_playback_end(ai_ptt *ptt, uint32_t generation, uint32_t now_ms);
void ai_ptt_response_end(ai_ptt *ptt, uint32_t generation);
uint32_t ai_ptt_cancel(ai_ptt *ptt);

#endif
