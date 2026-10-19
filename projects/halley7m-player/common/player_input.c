#include "player_input.h"
#include <string.h>

/* Repeats leave room for all three keys to produce one-shot actions. */
#define ACTION_RESERVE 3u

static void remove_event(player_input_t *input, uint32_t index)
{
    --input->count;
    memmove(input->events + index, input->events + index + 1u,
            (input->count - index) * sizeof(input->events[0]));
}

static void emit(player_input_t *input, player_input_action_t action,
                  uint32_t now_ms, bool repeat)
{
    if (repeat && input->count >= PLAYER_INPUT_CAPACITY - ACTION_RESERVE) {
        ++input->dropped_repeats;
        return;
    }
    if (input->count == PLAYER_INPUT_CAPACITY) {
        uint32_t index;
        for (index = 0; index < input->count; ++index)
            if (input->events[index].repeat) break;
        if (index == input->count) {
            ++input->dropped_actions;
            return;
        }
        remove_event(input, index);
        ++input->dropped_repeats;
    }
    player_input_event_t *event = input->events + input->count++;
    event->action = action;
    event->timestamp_ms = now_ms;
    event->generation = input->generation;
    event->repeat = repeat;
}

void player_input_init(player_input_t *input)
{
    memset(input, 0, sizeof(*input));
}

void player_input_set_generation(player_input_t *input, uint32_t generation)
{
    if (input->generation == generation) return;
    input->generation = generation;
    input->count = 0;
    input->repeating = 0;
    input->start_long = false;
    input->release_gate = input->held != 0;
}

void player_input_sample(player_input_t *input, uint16_t stable_keys,
                         uint32_t now_ms)
{
    static const uint16_t key_bits[3] = {
        PLAYER_INPUT_KEY_A, PLAYER_INPUT_KEY_B, PLAYER_INPUT_KEY_START
    };
    uint16_t keys = stable_keys & PLAYER_INPUT_KEY_MASK;
    uint16_t previous = input->held;
    input->held = keys;
    if (input->release_gate) {
        if (!keys) input->release_gate = false;
        return;
    }

    for (unsigned index = 0; index < 3; ++index) {
        uint16_t bit = key_bits[index];
        if ((keys & bit) && !(previous & bit)) {
            input->pressed_at[index] = now_ms;
            if (index < 2) {
                input->repeating &= (uint16_t)~bit;
                emit(input, index == 0 ? PLAYER_INPUT_A : PLAYER_INPUT_B,
                     now_ms, false);
            } else input->start_long = false;
        }
        if (index < 2) {
            if (!(keys & bit)) input->repeating &= (uint16_t)~bit;
            else if (previous & bit) {
                uint32_t elapsed = now_ms - ((input->repeating & bit) ?
                    input->repeated_at[index] : input->pressed_at[index]);
                uint32_t interval = (input->repeating & bit) ?
                    PLAYER_INPUT_INTERVAL_MS : PLAYER_INPUT_REPEAT_MS;
                if (elapsed >= interval) {
                    input->repeating |= bit;
                    input->repeated_at[index] = now_ms;
                    emit(input, index == 0 ? PLAYER_INPUT_A : PLAYER_INPUT_B,
                         now_ms, true);
                }
            }
        } else if (previous & bit) {
            if (!input->start_long &&
                now_ms - input->pressed_at[index] >= PLAYER_INPUT_LONG_MS) {
                emit(input, PLAYER_INPUT_START_LONG, now_ms, false);
                input->start_long = true;
            }
            if (!(keys & bit) && !input->start_long)
                emit(input, PLAYER_INPUT_START_SHORT, now_ms, false);
        }
    }
}

bool player_input_pop(player_input_t *input, player_input_event_t *event)
{
    if (!input->count) return false;
    *event = input->events[0];
    remove_event(input, 0);
    return true;
}
