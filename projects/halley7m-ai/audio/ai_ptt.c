#include "ai_ptt.h"
#include <string.h>

static void next_generation(ai_ptt *p)
{
    if (++p->generation == 0) ++p->generation;
}

void ai_ptt_init(ai_ptt *p)
{
    if (p) memset(p, 0, sizeof(*p));
}

int ai_ptt_set_tail_delay(ai_ptt *p, uint32_t measured_ms)
{
    if (!p || (p->state != AI_PTT_IDLE && p->state != AI_PTT_STOPPING) ||
        measured_ms >= UINT32_C(0x80000000)) return -1;
    p->tail_delay_ms = measured_ms;
    p->tail_delay_known = true;
    return 0;
}

static uint32_t start_capture(ai_ptt *p, uint32_t now)
{
    p->state = AI_PTT_RECORDING;
    p->capture_started_ms = now;
    p->captured_samples = 0;
    return AI_PTT_START_CAPTURE;
}

static uint32_t finish_capture(ai_ptt *p)
{
    p->state = p->captured_samples ? AI_PTT_WAITING : AI_PTT_IDLE;
    return AI_PTT_STOP_CAPTURE | (p->captured_samples ?
           AI_PTT_SUBMIT_CAPTURE : AI_PTT_DISCARD_CAPTURE);
}

uint32_t ai_ptt_tick(ai_ptt *p, uint32_t now)
{
    if (!p) return 0;
    if (p->may_be_audible && p->stop_pending && p->tail_delay_known &&
        (uint32_t)(now - p->stop_started_ms) >= p->tail_delay_ms) {
        p->may_be_audible = false;
        p->stop_pending = false;
    }
    if (p->state == AI_PTT_STOPPING && !p->may_be_audible) {
        if (p->held) return start_capture(p, now);
        p->state = AI_PTT_IDLE;
    }
    if (p->state == AI_PTT_RECORDING &&
        (uint32_t)(now - p->capture_started_ms) >= AI_PTT_MAX_MS)
        return finish_capture(p);
    return 0;
}

uint32_t ai_ptt_press(ai_ptt *p, uint32_t now)
{
    if (!p || p->held) return 0;
    uint32_t actions = 0;
    if (p->state == AI_PTT_WAITING || p->state == AI_PTT_PLAYING)
        actions |= AI_PTT_CANCEL_RESPONSE;
    p->held = true;
    next_generation(p);
    p->captured_samples = 0;
    if (p->may_be_audible) {
        p->state = AI_PTT_STOPPING;
        if (!p->stop_pending) actions |= AI_PTT_STOP_PLAYBACK;
        if (!p->tail_delay_known) actions |= AI_PTT_NEED_STOP_TIMING;
        return actions | ai_ptt_tick(p, now);
    }
    return actions | start_capture(p, now);
}

uint32_t ai_ptt_release(ai_ptt *p, uint32_t now)
{
    if (!p || !p->held) return 0;
    p->held = false;
    if (p->state == AI_PTT_RECORDING) return finish_capture(p);
    if (p->state == AI_PTT_STOPPING) p->state = AI_PTT_IDLE;
    return ai_ptt_tick(p, now);
}

void ai_ptt_playback_stopped(ai_ptt *p, uint32_t generation, uint32_t now)
{
    if (!p || !p->may_be_audible || generation != p->generation ||
        (p->state != AI_PTT_IDLE && p->state != AI_PTT_STOPPING)) return;
    p->stop_started_ms = now;
    p->stop_pending = true;
}

uint32_t ai_ptt_capture(ai_ptt *p, uint32_t samples, uint32_t *accepted)
{
    if (accepted) *accepted = 0;
    if (!p || !accepted || p->state != AI_PTT_RECORDING) return 0;
    uint32_t remaining = AI_PTT_MAX_SAMPLES - p->captured_samples;
    *accepted = samples < remaining ? samples : remaining;
    p->captured_samples += *accepted;
    return p->captured_samples == AI_PTT_MAX_SAMPLES ? finish_capture(p) : 0;
}

bool ai_ptt_response_current(const ai_ptt *p, uint32_t generation)
{
    return p && generation && generation == p->generation &&
           (p->state == AI_PTT_WAITING || p->state == AI_PTT_PLAYING);
}

bool ai_ptt_playback_begin(ai_ptt *p, uint32_t generation)
{
    if (!ai_ptt_response_current(p, generation)) return false;
    p->state = AI_PTT_PLAYING;
    p->may_be_audible = true;
    p->stop_pending = false;
    return true;
}

void ai_ptt_playback_end(ai_ptt *p, uint32_t generation, uint32_t now)
{
    if (!p || generation != p->generation || p->state != AI_PTT_PLAYING) return;
    p->state = AI_PTT_IDLE;
    ai_ptt_playback_stopped(p, generation, now);
}

void ai_ptt_response_end(ai_ptt *p, uint32_t generation)
{
    if (p && generation == p->generation && p->state == AI_PTT_WAITING)
        p->state = AI_PTT_IDLE;
}

uint32_t ai_ptt_cancel(ai_ptt *p)
{
    if (!p) return 0;
    uint32_t actions = 0;
    if (p->state == AI_PTT_RECORDING)
        actions |= AI_PTT_STOP_CAPTURE | AI_PTT_DISCARD_CAPTURE;
    if (p->state == AI_PTT_WAITING || p->state == AI_PTT_PLAYING)
        actions |= AI_PTT_CANCEL_RESPONSE;
    if (p->may_be_audible && !p->stop_pending) actions |= AI_PTT_STOP_PLAYBACK;
    next_generation(p);
    p->state = AI_PTT_IDLE;
    p->captured_samples = 0;
    /* Preserve held: another press cannot restart until the physical release. */
    return actions;
}
