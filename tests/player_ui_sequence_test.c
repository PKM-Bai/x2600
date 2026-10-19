#include "player_ui_sequence.h"
#include "player_input.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    player_ui_sequence_t sequence;
    player_gui_stats_t gui;
    player_input_t input;
    uint32_t base;
    unsigned events, pause_actions, volume_actions, scroll;
    uint32_t event_time[128];
    player_input_action_t event_action[128];
    bool event_repeat[128];
} test_run;

static void change_context(test_run *run)
{
    ++run->gui.generation;
    player_input_set_generation(&run->input, run->gui.generation);
}

/* Only the GUI action rules are modelled here; keys are interpreted by the
 * real player_input.c, including the real context-change release gate. */
static void sample(test_run *run, uint32_t elapsed, uint16_t physical)
{
    uint32_t now = run->base + elapsed;
    uint16_t injected = player_ui_sequence_sample(&run->sequence, now, physical, &run->gui);
    if (injected & (PLAYER_INPUT_KEY_A | PLAYER_INPUT_KEY_B))
        assert(run->gui.menu_open || run->gui.page == PLAYER_GUI_DIAGNOSTICS);
    player_input_sample(&run->input, injected | physical, now);
    player_input_event_t event;
    while (player_input_pop(&run->input, &event)) {
        assert(run->events < 128u);
        run->event_time[run->events] = event.timestamp_ms - run->base;
        run->event_action[run->events] = event.action;
        run->event_repeat[run->events++] = event.repeat;
        if (event.action == PLAYER_INPUT_START_LONG) {
            assert(!run->gui.menu_open && run->gui.page == PLAYER_GUI_PLAYBACK);
            run->gui.menu_open = 1;
            run->gui.menu_selected = 0;
            change_context(run);
        } else if (run->gui.menu_open) {
            if (event.action == PLAYER_INPUT_B) ++run->gui.menu_selected;
            else if (event.action == PLAYER_INPUT_START_SHORT) {
                assert(run->gui.menu_selected == 1);
                run->gui.menu_open = 0;
                run->gui.page = PLAYER_GUI_DIAGNOSTICS;
                change_context(run);
            } else assert(0);
        } else if (run->gui.page == PLAYER_GUI_DIAGNOSTICS) {
            if (event.action == PLAYER_INPUT_B) { if (run->scroll < 3u) ++run->scroll; }
            else if (event.action == PLAYER_INPUT_A) { if (run->scroll) --run->scroll; }
            else {
                assert(event.action == PLAYER_INPUT_START_SHORT);
                run->gui.page = PLAYER_GUI_PLAYBACK;
                change_context(run);
            }
        } else if (event.action == PLAYER_INPUT_START_SHORT) ++run->pause_actions;
        else ++run->volume_actions;
    }
}

static void begin(test_run *run, uint32_t base, uint32_t seconds)
{
    memset(run, 0, sizeof(*run));
    run->base = base;
    run->gui.page = PLAYER_GUI_PLAYBACK;
    run->gui.generation = 1;
    player_input_init(&run->input);
    player_input_set_generation(&run->input, 1);
    assert(player_ui_sequence_start(&run->sequence, base, seconds) == 0);
}

static void advance(test_run *run, uint32_t first, uint32_t last)
{
    for (uint32_t time = first; time <= last; time += 5u) sample(run, time, 0);
}

static void check_normal(uint32_t base, uint32_t seconds, unsigned cycles)
{
    test_run run;
    begin(&run, base, seconds);
    advance(&run, 0, seconds * 1000u);
    player_ui_sequence_status_t status;
    player_ui_sequence_get_status(&run.sequence, &status);
    assert(!status.active && status.reason == PLAYER_UI_SEQUENCE_COMPLETE);
    assert(status.cycles_started == cycles && status.cycles_completed == cycles);
    assert(!status.aborted && !status.cancel_may_emit_short && !status.keys);
    assert(!run.pause_actions && !run.volume_actions && !run.scroll);
    assert(run.gui.page == PLAYER_GUI_PLAYBACK && !run.gui.menu_open);
    assert(run.events == 8u * cycles);
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        unsigned i = cycle * 8u;
        uint32_t start = 30000u + cycle * 120000u;
        assert(run.event_action[i] == PLAYER_INPUT_START_LONG && run.event_time[i] == start + 800u);
        assert(run.event_action[i+1] == PLAYER_INPUT_B && run.event_time[i+1] == start + 1000u);
        assert(run.event_action[i+2] == PLAYER_INPUT_START_SHORT && run.event_time[i+2] == start + 1450u);
        assert(run.event_action[i+3] == PLAYER_INPUT_B && !run.event_repeat[i+3]);
        assert(run.event_action[i+4] == PLAYER_INPUT_B && run.event_repeat[i+4]);
        assert(run.event_time[i+4] - run.event_time[i+3] == 350u);
        assert(run.event_action[i+5] == PLAYER_INPUT_A && !run.event_repeat[i+5]);
        assert(run.event_action[i+6] == PLAYER_INPUT_A && run.event_repeat[i+6]);
        assert(run.event_time[i+6] - run.event_time[i+5] == 350u);
        assert(run.event_action[i+7] == PLAYER_INPUT_START_SHORT && run.event_time[i+7] == start + 3150u);
    }
    player_ui_sequence_stop(&run.sequence);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_COMPLETE);
}

int main(void)
{
    check_normal(0, 600, 5);
    check_normal(UINT32_MAX - 30300u, 1800, 15);

    test_run run;
    begin(&run, 0, 600);
    assert(!run.sequence.status.keys);
    assert(player_ui_sequence_start(&run.sequence, 0, 600) == -EBUSY);
    advance(&run, 0, 29995);
    run.gui.page = PLAYER_GUI_LIBRARY;
    sample(&run, 30000, 0);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_GUI_STATE && !run.events);

    begin(&run, 0, 600);
    advance(&run, 0, 30400);
    sample(&run, 30405, PLAYER_INPUT_KEY_START); /* Physical Start remains held. */
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_PHYSICAL_KEY && run.sequence.status.aborted);
    assert(run.sequence.status.cancel_may_emit_short && !run.sequence.status.keys);
    sample(&run, 30410, 0); /* Real short-release semantics are NOT hidden. */
    assert(run.pause_actions == 1);

    begin(&run, 0, 600);
    advance(&run, 0, 30400);
    player_ui_sequence_stop(&run.sequence);
    assert(run.sequence.status.cancel_may_emit_short && run.sequence.status.aborted);
    sample(&run, 30405, 0);
    assert(run.pause_actions == 1 && run.sequence.status.reason == PLAYER_UI_SEQUENCE_STOPPED);

    begin(&run, 0, 600);
    advance(&run, 0, 30100);
    sample(&run, 30205, 0); /* No catch-up key replay after delayed callback. */
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_SAMPLE_DELAY);
    assert(run.sequence.status.cancel_may_emit_short && !run.sequence.status.keys && run.pause_actions == 1);

    begin(&run, 0, 600);
    advance(&run, 0, 30995);
    run.gui.menu_open = 0; /* Never send B to a playback page without menu. */
    sample(&run, 31000, 0);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_GUI_STATE && !run.volume_actions);

    begin(&run, 0, 600);
    advance(&run, 0, 30995);
    run.gui.menu_selected = 2; /* Never confirm an unexpected menu choice. */
    sample(&run, 31000, 0);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_GUI_STATE);

    begin(&run, 0, 31);
    advance(&run, 0, 31000);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_DURATION_IN_CYCLE && run.sequence.status.aborted);

    begin(&run, 0, 600);
    sample(&run, 0, PLAYER_INPUT_KEY_B);
    assert(run.sequence.status.reason == PLAYER_UI_SEQUENCE_PHYSICAL_KEY && !run.sequence.status.keys);
    assert(!run.sequence.status.cancel_may_emit_short);

    memset(&run, 0, sizeof(run));
    assert(player_ui_sequence_start(NULL, 0, 600) == -EINVAL);
    assert(player_ui_sequence_start(&run.sequence, 0, 0) == -EINVAL);
    assert(player_ui_sequence_start(&run.sequence, 0, 1801) == -EINVAL);
    puts("PASS UI validation sequence + real input: 600/1800s cycles, exact actions/repeats, uint32 wrap, wrong-page/menu guards, physical/stop/delay release side effects, no normal pause/volume actions");
    return 0;
}
