#include "buttons.h"
#include <string.h>

#ifndef BUTTONS_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "x2600_hal.h"
#include <errno.h>
#define DEBOUNCE_TICKS ((uint32_t)pdMS_TO_TICKS(20))
#else
#define DEBOUNCE_TICKS 20u
#endif

typedef struct {
    uint16_t raw;
    uint16_t stable;
    uint16_t candidate;
    uint32_t changed_at[3];
    uint32_t presses[3];
    uint32_t releases[3];
} button_state_t;

static button_state_t state;
static buttons_sample_callback_t sample_callback;
static const uint16_t key_bits[3] = {
    BUTTONS_KEY_START, BUTTONS_KEY_A, BUTTONS_KEY_B
};

/* Each input has its own settling interval, so one bouncing key cannot hold
 * up another key. Unsigned subtraction also handles the tick counter wrap. */
static void sample_buttons(uint16_t raw, uint32_t now)
{
    unsigned i;
    state.raw = raw & BUTTONS_KEY_MASK;
    for (i = 0; i < 3; ++i) {
        uint16_t bit = key_bits[i];
        if ((state.raw ^ state.candidate) & bit) {
            state.candidate ^= bit;
            state.changed_at[i] = now;
        } else if (((state.candidate ^ state.stable) & bit) &&
                   (uint32_t)(now - state.changed_at[i]) >= DEBOUNCE_TICKS) {
            state.stable ^= bit;
            if (state.stable & bit)
                ++state.presses[i];
            else
                ++state.releases[i];
        }
    }
}

void buttons_set_sample_callback(buttons_sample_callback_t callback)
{
#ifndef BUTTONS_HOST_TEST
    taskENTER_CRITICAL();
#endif
    sample_callback = callback;
#ifndef BUTTONS_HOST_TEST
    taskEXIT_CRITICAL();
#endif
}

#ifdef BUTTONS_HOST_TEST
void buttons_test_reset(void)
{
    memset(&state, 0, sizeof(state));
}

void buttons_test_sample(uint16_t raw, uint32_t now_ms)
{
    sample_buttons(raw, now_ms);
    if (sample_callback) sample_callback(now_ms, state.stable);
}

void buttons_test_stats(buttons_test_stats_t *stats)
{
    stats->raw = state.raw;
    stats->stable = state.stable;
    memcpy(stats->presses, state.presses, sizeof(stats->presses));
    memcpy(stats->releases, state.releases, sizeof(stats->releases));
}

uint16_t buttons_get_keys(void)
{
    return state.stable;
}
#else
static TaskHandle_t buttons_task_handle;

static uint16_t read_keys(void)
{
    /* One bank read samples combinations together. The schematic has external
     * 10K pull-ups to 1V8P and 0.1uF capacitors; each switch shorts to ground. */
    uint32_t pressed = ~LL_GPIO_getPinMaskLevel(PE_Instance);
    uint16_t keys = 0;
    if (pressed & (1u << 13)) keys |= BUTTONS_KEY_START; /* SW6. */
    if (pressed & (1u << 14)) keys |= BUTTONS_KEY_A;     /* SW3. */
    if (pressed & (1u << 15)) keys |= BUTTONS_KEY_B;     /* SW4. */
    return keys;
}

static void buttons_task(void *argument)
{
    TickType_t wake = xTaskGetTickCount();
    (void)argument;
    for (;;) {
        uint16_t raw = read_keys();
        uint32_t now = (uint32_t)xTaskGetTickCount();
        taskENTER_CRITICAL();
        sample_buttons(raw, now);
        buttons_sample_callback_t callback = sample_callback;
        uint16_t stable = state.stable;
        taskEXIT_CRITICAL();
        if (callback) callback((uint32_t)(platform_time_us() / 1000u), stable);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    }
}

int buttons_start(void)
{
    unsigned pin;
    if (buttons_task_handle)
        return 0;
    for (pin = 13; pin <= 15; ++pin) {
        LL_GPIO_setPinMode(PE_Instance, pin, GPIO_MODE_INPUT);
        LL_GPIO_setPinPull(PE_Instance, pin, 0); /* External 1.8V pull-up. */
    }
    memset(&state, 0, sizeof(state));
    if (xTaskCreate(buttons_task, "buttons", 768, NULL, 4,
                    &buttons_task_handle) != pdPASS)
        return -ENOMEM;
    return 0;
}

uint16_t buttons_get_keys(void)
{
    uint16_t keys;
    taskENTER_CRITICAL();
    keys = state.stable;
    taskEXIT_CRITICAL();
    return keys;
}

void buttons_status(void)
{
    button_state_t snapshot;
    taskENTER_CRITICAL();
    snapshot = state;
    taskEXIT_CRITICAL();
    prom_printk("BUTTONS running=%u raw=%03x stable=%03x debounce_ms=20 sample_ms=5 "
                "Start_PE13=%lu/%lu A_PE14=%lu/%lu B_PE15=%lu/%lu (press/release)\n",
                buttons_task_handle != NULL, (unsigned)snapshot.raw,
                (unsigned)snapshot.stable,
                (unsigned long)snapshot.presses[0], (unsigned long)snapshot.releases[0],
                (unsigned long)snapshot.presses[1], (unsigned long)snapshot.releases[1],
                (unsigned long)snapshot.presses[2], (unsigned long)snapshot.releases[2]);
}
#endif
