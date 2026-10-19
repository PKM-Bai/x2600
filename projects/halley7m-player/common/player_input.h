/* Three-button player actions. Input masks match the existing GPIO driver. */
#ifndef PLAYER_INPUT_H
#define PLAYER_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define PLAYER_INPUT_KEY_A       0x001u
#define PLAYER_INPUT_KEY_B       0x002u
#define PLAYER_INPUT_KEY_START   0x008u
#define PLAYER_INPUT_KEY_MASK    0x00bu
#define PLAYER_INPUT_CAPACITY    32u
#define PLAYER_INPUT_REPEAT_MS   350u
#define PLAYER_INPUT_INTERVAL_MS 100u
#define PLAYER_INPUT_LONG_MS     800u

typedef enum {
    PLAYER_INPUT_A,
    PLAYER_INPUT_B,
    PLAYER_INPUT_START_SHORT,
    PLAYER_INPUT_START_LONG
} player_input_action_t;

typedef struct {
    player_input_action_t action;
    uint32_t timestamp_ms;
    uint32_t generation;
    bool repeat;
} player_input_event_t;

/* Statically allocated; callers must serialize sample/pop/context changes.
 * Counters survive context changes so queue pressure remains observable. */
typedef struct {
    player_input_event_t events[PLAYER_INPUT_CAPACITY];
    uint32_t count;
    uint32_t generation;
    uint32_t pressed_at[3];
    uint32_t repeated_at[2];
    uint32_t dropped_repeats;
    uint32_t dropped_actions;
    uint16_t held;
    uint16_t repeating;
    bool start_long;
    bool release_gate;
} player_input_t;

void player_input_init(player_input_t *input);

/* Feed the GPIO driver's already debounced mask, normally every 5ms.
 * A/B act immediately, then repeat after 350ms and every 100ms. A delayed
 * sample emits at most one repeat per key; missed repeats are not replayed.
 * Start emits on release, or once after 800ms with no short action on release.
 * Unsigned elapsed times tolerate the uint32_t millisecond clock wrapping. */
void player_input_sample(player_input_t *input, uint16_t stable_keys,
                         uint32_t now_ms);
bool player_input_pop(player_input_t *input, player_input_event_t *event);

/* On a new page/mode, discard queued old-context actions. Keys currently
 * held must all be released before any new action. Use a new generation
 * for each transition; passing the current generation is a no-op. */
void player_input_set_generation(player_input_t *input, uint32_t generation);

#endif
