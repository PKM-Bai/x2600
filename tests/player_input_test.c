/* Exercise the production action recognizer with deterministic stable inputs. */
#include "player_input.h"
#include <stdio.h>

static unsigned failures;

static void expect(bool condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL player_input: %s\n", name);
        ++failures;
    }
}

static void expect_event(player_input_t *input, player_input_action_t action,
                         uint32_t timestamp, bool repeat, uint32_t generation)
{
    player_input_event_t event;
    if (!player_input_pop(input, &event)) {
        expect(false, "expected queued event");
        return;
    }
    expect(event.action == action, "event action");
    expect(event.timestamp_ms == timestamp, "event timestamp");
    expect(event.repeat == repeat, "event repeat flag");
    expect(event.generation == generation, "event generation");
    expect(input->count <= PLAYER_INPUT_CAPACITY, "bounded queue");
}

static void expect_empty(player_input_t *input)
{
    player_input_event_t event;
    expect(!player_input_pop(input, &event), "no extra event");
    expect(input->count == 0, "empty count");
}

static void check_short_and_repeated_clicks(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, 0x8000, 0);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 5);
    expect_event(&input, PLAYER_INPUT_A, 5, false, 0);
    player_input_sample(&input, 0, 10);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_B, 15);
    expect_event(&input, PLAYER_INPUT_B, 15, false, 0);
    player_input_sample(&input, 0, 20);
    expect_empty(&input);

    /* Closely spaced clicks are independent; no double-click delay. */
    for (uint32_t index = 0; index < 12; ++index) {
        uint32_t time = 100u + index * 10u;
        player_input_sample(&input, PLAYER_INPUT_KEY_START, time);
        player_input_sample(&input, 0, time + 5u);
    }
    expect(input.count == 12, "all rapid clicks queued");
    for (uint32_t index = 0; index < 12; ++index)
        expect_event(&input, PLAYER_INPUT_START_SHORT,
                     105u + index * 10u, false, 0);
    expect_empty(&input);
}

static void check_repeat_timing(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 10);
    expect_event(&input, PLAYER_INPUT_A, 10, false, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 359);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 360);
    expect_event(&input, PLAYER_INPUT_A, 360, true, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 459);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 460);
    expect_event(&input, PLAYER_INPUT_A, 460, true, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 10000);
    expect_event(&input, PLAYER_INPUT_A, 10000, true, 0);
    expect_empty(&input); /* A late sample must not replay old repeats. */
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 10099);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 10100);
    expect_event(&input, PLAYER_INPUT_A, 10100, true, 0);
    player_input_sample(&input, 0, 10200);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 10205);
    expect_event(&input, PLAYER_INPUT_A, 10205, false, 0);
    player_input_sample(&input, 0, 10555);
    expect_empty(&input); /* Release at repeat threshold adds nothing. */
}

static void check_start_threshold(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 100);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 898);
    expect_empty(&input);
    player_input_sample(&input, 0, 899);
    expect_event(&input, PLAYER_INPUT_START_SHORT, 899, false, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 1000);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 1799);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 1800);
    expect_event(&input, PLAYER_INPUT_START_LONG, 1800, false, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 5000);
    player_input_sample(&input, 0, 5005);
    expect_empty(&input);
    /* The release sample itself can be the first observation at 800ms. */
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 6000);
    player_input_sample(&input, 0, 6800);
    expect_event(&input, PLAYER_INPUT_START_LONG, 6800, false, 0);
    expect_empty(&input);
}

static void check_independent_keys_and_wrap(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_MASK, 0);
    expect_event(&input, PLAYER_INPUT_A, 0, false, 0);
    expect_event(&input, PLAYER_INPUT_B, 0, false, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_B | PLAYER_INPUT_KEY_START, 350);
    expect_event(&input, PLAYER_INPUT_B, 350, true, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 450);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 800);
    expect_event(&input, PLAYER_INPUT_START_LONG, 800, false, 0);
    player_input_sample(&input, 0, 805);
    expect_empty(&input);

    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, UINT32_MAX - 100u);
    expect_event(&input, PLAYER_INPUT_A, UINT32_MAX - 100u, false, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 248);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 249);
    expect_event(&input, PLAYER_INPUT_A, 249, true, 0);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 349);
    expect_event(&input, PLAYER_INPUT_A, 349, true, 0);
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, UINT32_MAX - 200u);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 598);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 599);
    expect_event(&input, PLAYER_INPUT_START_LONG, 599, false, 0);
    player_input_sample(&input, 0, 600);
    expect_empty(&input);
}

static void check_context_release_gate(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A | PLAYER_INPUT_KEY_START, 0);
    player_input_set_generation(&input, 1);
    expect_empty(&input); /* The previous page's pending A is discarded. */
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 1000);
    player_input_sample(&input, PLAYER_INPUT_KEY_B, 1010);
    expect_empty(&input); /* All keys, including newly pressed B, are gated. */
    player_input_sample(&input, 0, 1020);
    expect_empty(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_B, 1030);
    player_input_set_generation(&input, 1);
    expect_event(&input, PLAYER_INPUT_B, 1030, false, 1);
    player_input_sample(&input, 0, 1040);
    player_input_set_generation(&input, 2);
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 1050);
    player_input_sample(&input, 0, 1100);
    expect_event(&input, PLAYER_INPUT_START_SHORT, 1100, false, 2);
    expect_empty(&input);
}

static void check_queue_priority(void)
{
    player_input_t input;
    player_input_init(&input);
    player_input_sample(&input, PLAYER_INPUT_KEY_A, 0);
    expect_event(&input, PLAYER_INPUT_A, 0, false, 0);
    for (uint32_t index = 0; index < 100; ++index)
        player_input_sample(&input, PLAYER_INPUT_KEY_A, 350u + index * 100u);
    expect(input.count == 29, "repeat reserve leaves three slots");
    expect(input.dropped_repeats == 71, "repeat pressure counted");
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 10400);
    player_input_sample(&input, 0, 10500);
    player_input_sample(&input, PLAYER_INPUT_KEY_B, 10600);
    player_input_sample(&input, 0, 10700);
    player_input_sample(&input, PLAYER_INPUT_KEY_B, 10800);
    expect(input.count == 32, "one-shot actions use reserved slots");
    player_input_sample(&input, PLAYER_INPUT_KEY_START, 10900);
    player_input_sample(&input, 0, 11000);
    expect(input.count == 32, "full queue remains bounded");
    expect(input.dropped_repeats == 72, "one-shot evicts oldest repeat");
    expect(input.dropped_actions == 0, "no one-shot displaced by repeats");
    for (uint32_t index = 1; index < 29; ++index)
        expect_event(&input, PLAYER_INPUT_A, 350u + index * 100u, true, 0);
    expect_event(&input, PLAYER_INPUT_START_SHORT, 10500, false, 0);
    expect_event(&input, PLAYER_INPUT_B, 10600, false, 0);
    expect_event(&input, PLAYER_INPUT_B, 10800, false, 0);
    expect_event(&input, PLAYER_INPUT_START_SHORT, 11000, false, 0);
    expect_empty(&input);

    for (uint32_t index = 0; index < 33; ++index) {
        player_input_sample(&input, PLAYER_INPUT_KEY_A, 12000u + index * 10u);
        player_input_sample(&input, 0, 12005u + index * 10u);
    }
    expect(input.count == 32, "all-one-shot queue remains bounded");
    expect(input.dropped_actions == 1, "all-one-shot overflow is observable");
    for (uint32_t index = 0; index < 32; ++index)
        expect_event(&input, PLAYER_INPUT_A, 12000u + index * 10u, false, 0);
    player_input_set_generation(&input, 1);
    expect(input.dropped_actions == 1 && input.dropped_repeats == 72,
           "context change preserves overflow diagnostics");
    expect_empty(&input);
}

int main(void)
{
    check_short_and_repeated_clicks();
    check_repeat_timing();
    check_start_threshold();
    check_independent_keys_and_wrap();
    check_context_release_gate();
    check_queue_priority();
    if (failures) return 1;
    puts("player_input PASS: independent clicks, repeat 350/100ms, Start 800ms, "
         "tick wrap, context release gate, 32-event priority queue");
    return 0;
}
