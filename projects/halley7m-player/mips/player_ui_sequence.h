#ifndef PLAYER_UI_SEQUENCE_H
#define PLAYER_UI_SEQUENCE_H

#include "player_gui.h"
#include <stdint.h>

enum player_ui_sequence_reason {
    PLAYER_UI_SEQUENCE_IDLE,
    PLAYER_UI_SEQUENCE_RUNNING,
    PLAYER_UI_SEQUENCE_COMPLETE,
    PLAYER_UI_SEQUENCE_STOPPED,
    PLAYER_UI_SEQUENCE_PHYSICAL_KEY,
    PLAYER_UI_SEQUENCE_GUI_STATE,
    PLAYER_UI_SEQUENCE_SAMPLE_DELAY,
    PLAYER_UI_SEQUENCE_DURATION_IN_CYCLE
};

typedef struct player_ui_sequence_status {
    uint32_t active, reason, phase;
    uint32_t cycles_started, cycles_completed;
    uint32_t elapsed_ms, duration_ms, next_cycle_ms;
    uint32_t keys, aborted, cancel_may_emit_short;
} player_ui_sequence_status_t;

typedef struct player_ui_sequence {
    player_ui_sequence_status_t status;
    uint32_t started_ms, last_sample_ms, phase_started_ms;
} player_ui_sequence_t;

/* Pure validation input source. Zero-initialize ctx. The caller serializes
 * start/stop/status on the console task against the single 5ms sample callback
 * (short task critical sections suffice). No task, UART, GUI or session calls.
 * Start accepts 1..1800 seconds; first cycle begins at 30s, then every 120s.
 * sample returns an EXTRA stable mask, to OR with physical_keys before passing
 * the normal GUI recognizer. The first 30s and normal 600/1800s ends emit zero.
 * Any physical key, GUI error/unexpected page, or sample gap >100ms aborts.
 * Aborting/stopping while synthetic Start is held <800ms releases it and may
 * produce the real recognizer's short action: status explicitly records that
 * possibility. Any abort invalidates this run as a matching UI-sequence test.
 */
int player_ui_sequence_start(player_ui_sequence_t *ctx, uint32_t now_ms, uint32_t seconds);
void player_ui_sequence_stop(player_ui_sequence_t *ctx);
uint16_t player_ui_sequence_sample(player_ui_sequence_t *ctx, uint32_t now_ms,
                                   uint16_t physical_keys, const player_gui_stats_t *view);
void player_ui_sequence_get_status(const player_ui_sequence_t *ctx,
                                   player_ui_sequence_status_t *status);

#endif
