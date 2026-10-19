#ifndef REGBA_BUTTONS_H
#define REGBA_BUTTONS_H

#include <stdint.h>

/* Native GBA KEYINPUT bits, with 1 meaning pressed in this interface. */
#define BUTTONS_KEY_A     0x001u
#define BUTTONS_KEY_B     0x002u
#define BUTTONS_KEY_START 0x008u
#define BUTTONS_KEY_MASK  (BUTTONS_KEY_A | BUTTONS_KEY_B | BUTTONS_KEY_START)

int buttons_start(void);
uint16_t buttons_get_keys(void);
void buttons_status(void);
typedef void (*buttons_sample_callback_t)(uint32_t now_ms, uint16_t stable_keys);
/* Optional 5ms task-context consumer, called after the debounce lock is left.
 * Input bits above are already independently debounced for 20ms. NULL keeps
 * the original polling-only behavior. Callback must return promptly and may
 * not call LVGL or wait for media/storage work. Register before buttons_start.
 * Timestamps use platform_time_us()/1000, matching the player GUI clock. */
void buttons_set_sample_callback(buttons_sample_callback_t callback);

#ifdef BUTTONS_HOST_TEST
/* Host entry points exercise the same debounce state machine as the task. */
typedef struct {
    uint16_t raw;
    uint16_t stable;
    uint32_t presses[3];   /* Start, A, B. */
    uint32_t releases[3];
} buttons_test_stats_t;
void buttons_test_reset(void);
void buttons_test_sample(uint16_t raw, uint32_t now_ms);
void buttons_test_stats(buttons_test_stats_t *stats);
#endif

#endif
