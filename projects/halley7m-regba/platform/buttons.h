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
