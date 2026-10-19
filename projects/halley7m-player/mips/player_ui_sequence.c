#include "player_ui_sequence.h"
#include <errno.h>
#include <string.h>

enum sequence_phase {
    WAIT_CYCLE, MENU_START, MENU_RELEASE, MENU_B, MENU_B_RELEASE,
    ENTER_START, ENTER_RELEASE, DIAGNOSTIC_B, DIAGNOSTIC_B_RELEASE,
    DIAGNOSTIC_A, DIAGNOSTIC_A_RELEASE, RETURN_START, RETURN_RELEASE,
    PHASE_COUNT
};

static const struct { uint16_t keys, duration_ms; } phases[PHASE_COUNT] = {
    {0, 0}, {PLAYER_INPUT_KEY_START, 850}, {0, 150},
    {PLAYER_INPUT_KEY_B, 100}, {0, 250},
    {PLAYER_INPUT_KEY_START, 100}, {0, 300},
    {PLAYER_INPUT_KEY_B, 450}, {0, 200},
    {PLAYER_INPUT_KEY_A, 450}, {0, 200},
    {PLAYER_INPUT_KEY_START, 100}, {0, 300}
};

static void finish(player_ui_sequence_t *ctx, unsigned reason, unsigned aborted)
{
    if ((ctx->status.keys & PLAYER_INPUT_KEY_START) &&
        ctx->last_sample_ms - ctx->phase_started_ms < PLAYER_INPUT_LONG_MS)
        ctx->status.cancel_may_emit_short = 1;
    ctx->status.active = 0;
    ctx->status.keys = 0;
    ctx->status.reason = reason;
    ctx->status.aborted = aborted;
}

int player_ui_sequence_start(player_ui_sequence_t *ctx, uint32_t now_ms, uint32_t seconds)
{
    if (!ctx || !seconds || seconds > 1800u) return -EINVAL;
    if (ctx->status.active) return -EBUSY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->started_ms = ctx->last_sample_ms = now_ms;
    ctx->status.active = 1;
    ctx->status.reason = PLAYER_UI_SEQUENCE_RUNNING;
    ctx->status.duration_ms = seconds * 1000u;
    ctx->status.next_cycle_ms = 30000u;
    return 0;
}

void player_ui_sequence_stop(player_ui_sequence_t *ctx)
{
    if (ctx && ctx->status.active)
        finish(ctx, PLAYER_UI_SEQUENCE_STOPPED, ctx->status.phase != WAIT_CYCLE);
}

/* Checks allow only the expected asynchronous GUI transition during release
 * gaps. Every B/A/short-Start press has an exact page/menu entry guard. */
static int view_valid(unsigned phase, const player_gui_stats_t *view)
{
    if (!view || view->output_error) return 0;
    unsigned playback = view->page == PLAYER_GUI_PLAYBACK;
    unsigned diagnostic = view->page == PLAYER_GUI_DIAGNOSTICS;
    switch (phase) {
    case WAIT_CYCLE:
        return playback && !view->menu_open;
    case MENU_START: case MENU_RELEASE:
        return playback && (!view->menu_open || view->menu_selected == 0);
    case MENU_B: case MENU_B_RELEASE:
        return playback && view->menu_open && view->menu_selected <= 1u;
    case ENTER_START:
        return playback && view->menu_open && view->menu_selected == 1u;
    case ENTER_RELEASE:
        return (playback && view->menu_open && view->menu_selected == 1u) ||
               (diagnostic && !view->menu_open);
    case DIAGNOSTIC_B: case DIAGNOSTIC_B_RELEASE:
    case DIAGNOSTIC_A: case DIAGNOSTIC_A_RELEASE: case RETURN_START:
        return diagnostic && !view->menu_open;
    case RETURN_RELEASE:
        return (diagnostic || playback) && !view->menu_open;
    default:
        return 0;
    }
}

static int entry_valid(unsigned phase, const player_gui_stats_t *view)
{
    if (!view_valid(phase, view)) return 0;
    if (phase == MENU_START) return !view->menu_open;
    if (phase == MENU_B) return view->menu_selected == 0;
    return 1;
}

uint16_t player_ui_sequence_sample(player_ui_sequence_t *ctx, uint32_t now_ms,
                                   uint16_t physical_keys, const player_gui_stats_t *view)
{
    if (!ctx || !ctx->status.active) return 0;
    uint32_t gap = now_ms - ctx->last_sample_ms;
    ctx->last_sample_ms = now_ms;
    ctx->status.elapsed_ms = now_ms - ctx->started_ms;
    if (physical_keys & PLAYER_INPUT_KEY_MASK) {
        finish(ctx, PLAYER_UI_SEQUENCE_PHYSICAL_KEY, 1);
        return 0;
    }
    if (gap > 100u) {
        finish(ctx, PLAYER_UI_SEQUENCE_SAMPLE_DELAY, 1);
        return 0;
    }
    if (!view || view->output_error) {
        finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
        return 0;
    }
    if (ctx->status.elapsed_ms >= ctx->status.duration_ms) {
        unsigned in_cycle = ctx->status.phase != WAIT_CYCLE;
        if (!in_cycle && ctx->status.cycles_completed && !view_valid(WAIT_CYCLE, view)) {
            finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
            return 0;
        }
        finish(ctx, in_cycle ? PLAYER_UI_SEQUENCE_DURATION_IN_CYCLE : PLAYER_UI_SEQUENCE_COMPLETE,
               in_cycle);
        return 0;
    }
    unsigned phase = ctx->status.phase;
    if (phase == WAIT_CYCLE) {
        if (ctx->status.cycles_completed && !view_valid(WAIT_CYCLE, view)) {
            finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
            return 0;
        }
        if (ctx->status.elapsed_ms < ctx->status.next_cycle_ms) return 0;
        if (!entry_valid(MENU_START, view)) {
            finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
            return 0;
        }
        phase = MENU_START;
        ++ctx->status.cycles_started;
        ctx->phase_started_ms = now_ms;
    } else {
        if (!view_valid(phase, view)) {
            finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
            return 0;
        }
        if (now_ms - ctx->phase_started_ms >= phases[phase].duration_ms) {
            ++phase; /* Never replay missed key stages in one delayed sample. */
            if (phase == PHASE_COUNT) {
                if (!view_valid(WAIT_CYCLE, view)) {
                    finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
                    return 0;
                }
                ++ctx->status.cycles_completed;
                ctx->status.next_cycle_ms += 120000u;
                phase = WAIT_CYCLE;
            } else if (!entry_valid(phase, view)) {
                finish(ctx, PLAYER_UI_SEQUENCE_GUI_STATE, 1);
                return 0;
            }
            ctx->phase_started_ms = now_ms;
        }
    }
    ctx->status.phase = phase;
    ctx->status.keys = phases[phase].keys;
    return (uint16_t)ctx->status.keys;
}

void player_ui_sequence_get_status(const player_ui_sequence_t *ctx,
                                   player_ui_sequence_status_t *status)
{
    if (ctx && status) *status = ctx->status;
}
