/* Exercise the production debounce logic without GPIO or FreeRTOS. */
#include "buttons.h"
#include <stdint.h>
#include <stdio.h>

static unsigned failures;

static void expect(const char *name, uint16_t raw, uint16_t stable,
                   uint32_t start_press, uint32_t a_press, uint32_t b_press,
                   uint32_t start_release, uint32_t a_release, uint32_t b_release)
{
    buttons_test_stats_t s;
    buttons_test_stats(&s);
    if (s.raw != raw || s.stable != stable || buttons_get_keys() != stable ||
        s.presses[0] != start_press || s.presses[1] != a_press || s.presses[2] != b_press ||
        s.releases[0] != start_release || s.releases[1] != a_release || s.releases[2] != b_release) {
        printf("FAIL %s raw=%03x stable=%03x counts=%lu/%lu %lu/%lu %lu/%lu\n",
               name, (unsigned)s.raw, (unsigned)s.stable,
               (unsigned long)s.presses[0], (unsigned long)s.releases[0],
               (unsigned long)s.presses[1], (unsigned long)s.releases[1],
               (unsigned long)s.presses[2], (unsigned long)s.releases[2]);
        ++failures;
    }
}

int main(void)
{
    uint32_t t;
    buttons_test_reset();
    buttons_test_sample(0, 0);
    buttons_test_sample(BUTTONS_KEY_START, 5);
    buttons_test_sample(0, 10);
    buttons_test_sample(BUTTONS_KEY_START, 15);
    buttons_test_sample(0, 20);
    buttons_test_sample(0, 40);
    expect("short bounce", 0, 0, 0, 0, 0, 0, 0, 0);

    buttons_test_sample(BUTTONS_KEY_START, 45);
    buttons_test_sample(BUTTONS_KEY_START, 64);
    expect("19ms boundary", BUTTONS_KEY_START, 0, 0, 0, 0, 0, 0, 0);
    buttons_test_sample(BUTTONS_KEY_START, 65);
    expect("20ms boundary", BUTTONS_KEY_START, BUTTONS_KEY_START, 1, 0, 0, 0, 0, 0);
    for (t = 70; t < 10000; t += 5)
        buttons_test_sample(BUTTONS_KEY_START, t);
    expect("long hold no repeat", BUTTONS_KEY_START, BUTTONS_KEY_START, 1, 0, 0, 0, 0, 0);

    buttons_test_sample(0, 10000);
    buttons_test_sample(BUTTONS_KEY_START, 10005);
    buttons_test_sample(0, 10010);
    buttons_test_sample(0, 10029);
    expect("release bounce", 0, BUTTONS_KEY_START, 1, 0, 0, 0, 0, 0);
    buttons_test_sample(0, 10030);
    expect("release accepted", 0, 0, 1, 0, 0, 1, 0, 0);

    buttons_test_sample(BUTTONS_KEY_MASK, 10035);
    buttons_test_sample(BUTTONS_KEY_MASK, 10055);
    expect("all held together", BUTTONS_KEY_MASK, BUTTONS_KEY_MASK, 2, 1, 1, 1, 0, 0);
    buttons_test_sample(0, 10060);
    buttons_test_sample(0, 10080);
    expect("all released", 0, 0, 2, 1, 1, 2, 1, 1);

    buttons_test_reset();
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 0);
    buttons_test_sample(BUTTONS_KEY_A, 5);
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 10);
    buttons_test_sample(BUTTONS_KEY_A, 15);
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 20);
    expect("independent key bounce", BUTTONS_KEY_A | BUTTONS_KEY_B, BUTTONS_KEY_A, 0, 1, 0, 0, 0, 0);
    buttons_test_sample(BUTTONS_KEY_A | BUTTONS_KEY_B, 40);
    expect("second key accepted", BUTTONS_KEY_A | BUTTONS_KEY_B,
           BUTTONS_KEY_A | BUTTONS_KEY_B, 0, 1, 1, 0, 0, 0);

    buttons_test_reset();
    buttons_test_sample(BUTTONS_KEY_B | 0x8000u, UINT32_MAX - 9u);
    buttons_test_sample(BUTTONS_KEY_B, 9);
    expect("wrap 19ms", BUTTONS_KEY_B, 0, 0, 0, 0, 0, 0, 0);
    buttons_test_sample(BUTTONS_KEY_B, 10);
    expect("wrap 20ms and mask", BUTTONS_KEY_B, BUTTONS_KEY_B, 0, 0, 1, 0, 0, 0);
    if (failures) {
        printf("buttons regression: %u failure(s)\n", failures);
        return 1;
    }
    puts("buttons regression PASS: bounce, 20ms boundary, hold, release, combinations, independent keys, tick wrap");
    return 0;
}
