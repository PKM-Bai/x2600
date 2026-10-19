#include "buttons.h"
#include <assert.h>
#include <stdio.h>

static unsigned calls;
static uint32_t last_ms;
static uint16_t last_keys;
static void sample(uint32_t now_ms, uint16_t keys)
{
    ++calls;
    last_ms = now_ms;
    last_keys = keys;
    assert(buttons_get_keys() == keys);
}

int main(void)
{
    buttons_test_reset();
    buttons_set_sample_callback(sample);
    buttons_test_sample(0, 0);
    buttons_test_sample(BUTTONS_KEY_A, 5);
    assert(calls == 2 && last_ms == 5 && last_keys == 0);
    buttons_test_sample(BUTTONS_KEY_A, 24);
    assert(last_keys == 0);
    buttons_test_sample(BUTTONS_KEY_A, 25);
    assert(last_ms == 25 && last_keys == BUTTONS_KEY_A);
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 30);
    assert(last_keys == BUTTONS_KEY_A);
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 50);
    assert(last_keys == (BUTTONS_KEY_A | BUTTONS_KEY_B));
    unsigned before = calls;
    buttons_set_sample_callback(NULL);
    buttons_test_sample(0, 55);
    buttons_test_sample(0, 75);
    assert(calls == before && buttons_get_keys() == 0);
    puts("PASS buttons callback: stable mask/20ms/independent keys/timestamp/unregister; original polling preserved");
    return 0;
}
