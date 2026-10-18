/* Host regression for the production SBTCOM1 parser and GBA key mapping. */
#include "bt_gamepad.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

/* Wire bytes and expected GBA bits are literal, independent protocol fixtures. */
static const uint8_t neutral[] = {
    0xaa,0x55,0x0f,0xa0,0x01,0,0,0,0,0,0,0,0,0,0xaf
};
static const uint8_t a_pressed[] = {
    0xaa,0x55,0x0f,0xa0,0x01,0,0,0,0,0,1,0,0,0,0xb0
};
static const uint8_t all_pressed[] = {
    0xaa,0x55,0x0f,0xa0,0x01,0,0,0,0,0,0xc3,0x0c,0,0,0x7e
};
static const uint8_t link_up[] = {0xaa,0x55,7,0xa0,0,0x55,0xfb};
static const uint8_t link_down[] = {0xaa,0x55,7,0xa0,0,0xff,0xa5};

static void expect_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        printf("FAIL %s got=%lu expected=%lu\n", name,
               (unsigned long)actual, (unsigned long)expected);
        ++failures;
    }
}

static void expect_keys(const char *name, uint16_t expected)
{
    bt_gamepad_stats_t s;
    bt_gamepad_get_stats(&s);
    expect_u32(name, bt_gamepad_get_keys(), expected);
    expect_u32(name, s.keys, expected);
}

/* Fixtures retain the default orientation's literal golden masks. Swap only
 * those direction bits when board qualification reverses an axis sign. */
static uint16_t stick_expectation(uint16_t golden)
{
    uint16_t expected = golden;
#if BT_GAMEPAD_X_RIGHT_SIGN == -1
    expected = (uint16_t)((expected & ~0x030u) |
                         ((expected & 0x010u) << 1) |
                         ((expected & 0x020u) >> 1));
#endif
#if BT_GAMEPAD_Y_UP_SIGN == -1
    expected = (uint16_t)((expected & ~0x0c0u) |
                         ((expected & 0x040u) << 1) |
                         ((expected & 0x080u) >> 1));
#endif
    return expected;
}

static bt_gamepad_stats_t stats(void)
{
    bt_gamepad_stats_t s;
    bt_gamepad_get_stats(&s);
    return s;
}

static void feed(const uint8_t *bytes, unsigned count, uint32_t now)
{
    unsigned i;
    for (i = 0; i < count; ++i)
        bt_gamepad_test_feed(bytes[i], now);
}

static void test_key_contract_and_raw_data(void)
{
    static const uint8_t raw[] = {
        0xaa,0x55,0x0f,0xa0,0x01,5,0xfa,7,0xf8,0,0x18,0x63,0xe1,0x12,0x1b
    };
    bt_gamepad_stats_t s;
    expect_u32("GBA A bit", BT_GAMEPAD_KEY_A, 0x001);
    expect_u32("GBA B bit", BT_GAMEPAD_KEY_B, 0x002);
    expect_u32("GBA SELECT bit", BT_GAMEPAD_KEY_SELECT, 0x004);
    expect_u32("GBA START bit", BT_GAMEPAD_KEY_START, 0x008);
    expect_u32("GBA RIGHT bit", BT_GAMEPAD_KEY_RIGHT, 0x010);
    expect_u32("GBA LEFT bit", BT_GAMEPAD_KEY_LEFT, 0x020);
    expect_u32("GBA UP bit", BT_GAMEPAD_KEY_UP, 0x040);
    expect_u32("GBA DOWN bit", BT_GAMEPAD_KEY_DOWN, 0x080);
    expect_u32("GBA R bit", BT_GAMEPAD_KEY_R, 0x100);
    expect_u32("GBA L bit", BT_GAMEPAD_KEY_L, 0x200);
    bt_gamepad_test_reset();
    s = stats();
    expect_u32("reset initialized", s.initialized, 1);
    expect_u32("reset offline", s.connected, 0);
    expect_u32("reset invalid snapshot", s.data_valid, 0);
    expect_u32("reset data age", s.data_age_ms, UINT32_MAX);
    expect_keys("reset keys", 0);
    feed(raw, sizeof(raw), 100);
    s = stats();
    expect_u32("data implies online", s.connected, 1);
    expect_u32("data valid", s.data_valid, 1);
    expect_u32("one data frame", s.data_frames, 1);
    expect_u32("raw bytes counted", s.bytes, 15);
    expect_u32("raw lx", (uint32_t)(int32_t)s.lx, 5);
    expect_u32("signed raw ly", (uint32_t)(int32_t)s.ly, (uint32_t)-6);
    expect_u32("raw rx", (uint32_t)(int32_t)s.rx, 7);
    expect_u32("signed raw ry", (uint32_t)(int32_t)s.ry, (uint32_t)-8);
    expect_u32("raw key2", s.key2, 0x18);
    expect_u32("raw key3", s.key3, 0x63);
    expect_u32("right trigger position", s.rtrig, 0xe1);
    expect_u32("left trigger position", s.ltrig, 0x12);
    expect_keys("unused buttons and triggers do not map", 0);
}

struct mapping_case {
    const char *name;
    uint8_t frame[15];
    uint16_t expected;
};

static void test_button_and_direction_mapping(void)
{
    static const uint8_t return_to_stick[] = {
        0xaa,0x55,15,0xa0,1,0x30,0x30,0,0,0,0,0,0,0,0x0f
    };
    static const struct mapping_case cases[] = {
        {"A", {0xaa,0x55,15,0xa0,1,0,0,0,0,0,1,0,0,0,0xb0},0x001},
        {"B", {0xaa,0x55,15,0xa0,1,0,0,0,0,0,2,0,0,0,0xb1},0x002},
        {"LB",{0xaa,0x55,15,0xa0,1,0,0,0,0,0,0x40,0,0,0,0xef},0x200},
        {"RB",{0xaa,0x55,15,0xa0,1,0,0,0,0,0,0x80,0,0,0,0x2f},0x100},
        {"SELECT",{0xaa,0x55,15,0xa0,1,0,0,0,0,0,0,4,0,0,0xb3},0x004},
        {"START", {0xaa,0x55,15,0xa0,1,0,0,0,0,0,0,8,0,0,0xb7},0x008},
        {"dpad neutral",{0xaa,0x55,15,0xa0,1,0,0,0,0,0,0,0,0,0,0xaf},0},
        {"dpad up",{0xaa,0x55,15,0xa0,1,0,0,0,0,1,0,0,0,0,0xb0},0x040},
        {"dpad up right",{0xaa,0x55,15,0xa0,1,0,0,0,0,2,0,0,0,0,0xb1},0x050},
        {"dpad right",{0xaa,0x55,15,0xa0,1,0,0,0,0,3,0,0,0,0,0xb2},0x010},
        {"dpad down right",{0xaa,0x55,15,0xa0,1,0,0,0,0,4,0,0,0,0,0xb3},0x090},
        {"dpad down",{0xaa,0x55,15,0xa0,1,0,0,0,0,5,0,0,0,0,0xb4},0x080},
        {"dpad down left",{0xaa,0x55,15,0xa0,1,0,0,0,0,6,0,0,0,0,0xb5},0x0a0},
        {"dpad left",{0xaa,0x55,15,0xa0,1,0,0,0,0,7,0,0,0,0,0xb6},0x020},
        {"dpad up left",{0xaa,0x55,15,0xa0,1,0,0,0,0,8,0,0,0,0,0xb7},0x060},
        {"dpad wins over stick",{0xaa,0x55,15,0xa0,1,0x30,0x30,0,0,6,1,0,0,0,0x16},0x0a1}
    };
    unsigned i;
    bt_gamepad_test_reset();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        feed(cases[i].frame, sizeof(cases[i].frame), i);
        expect_keys(cases[i].name, cases[i].expected);
    }
    feed(return_to_stick, sizeof(return_to_stick), 30);
    expect_keys("return to stick", stick_expectation(0x050));
    feed(all_pressed, sizeof(all_pressed), 31);
    expect_keys("all six digital controls", 0x30f);
    feed(neutral, sizeof(neutral), 32);
    expect_keys("snapshot releases every key", 0);
}

static void test_stick_hysteresis(void)
{
    static const struct mapping_case cases[] = {
        {"x +47 idle",{0xaa,0x55,15,0xa0,1,47,0,0,0,0,0,0,0,0,0xde},0},
        {"x +48 press",{0xaa,0x55,15,0xa0,1,48,0,0,0,0,0,0,0,0,0xdf},0x010},
        {"x +33 hold",{0xaa,0x55,15,0xa0,1,33,0,0,0,0,0,0,0,0,0xd0},0x010},
        {"x +32 release",{0xaa,0x55,15,0xa0,1,32,0,0,0,0,0,0,0,0,0xcf},0},
        {"x -47 idle",{0xaa,0x55,15,0xa0,1,0xd1,0,0,0,0,0,0,0,0,0x80},0},
        {"x -48 press",{0xaa,0x55,15,0xa0,1,0xd0,0,0,0,0,0,0,0,0,0x7f},0x020},
        {"x -33 hold",{0xaa,0x55,15,0xa0,1,0xdf,0,0,0,0,0,0,0,0,0x8e},0x020},
        {"x -32 release",{0xaa,0x55,15,0xa0,1,0xe0,0,0,0,0,0,0,0,0,0x8f},0},
        {"y +47 idle",{0xaa,0x55,15,0xa0,1,0,47,0,0,0,0,0,0,0,0xde},0},
        {"y +48 press",{0xaa,0x55,15,0xa0,1,0,48,0,0,0,0,0,0,0,0xdf},0x040},
        {"y +33 hold",{0xaa,0x55,15,0xa0,1,0,33,0,0,0,0,0,0,0,0xd0},0x040},
        {"y +32 release",{0xaa,0x55,15,0xa0,1,0,32,0,0,0,0,0,0,0,0xcf},0},
        {"y -47 idle",{0xaa,0x55,15,0xa0,1,0,0xd1,0,0,0,0,0,0,0,0x80},0},
        {"y -48 press",{0xaa,0x55,15,0xa0,1,0,0xd0,0,0,0,0,0,0,0,0x7f},0x080},
        {"y -33 hold",{0xaa,0x55,15,0xa0,1,0,0xdf,0,0,0,0,0,0,0,0x8e},0x080},
        {"y -32 release",{0xaa,0x55,15,0xa0,1,0,0xe0,0,0,0,0,0,0,0,0x8f},0},
        {"two axes press",{0xaa,0x55,15,0xa0,1,48,48,0,0,0,0,0,0,0,0x0f},0x050},
        {"both axes direct reversal",{0xaa,0x55,15,0xa0,1,0xd0,0xd0,0,0,0,0,0,0,0,0x4f},0x0a0}
    };
    unsigned i;
    bt_gamepad_test_reset();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        feed(cases[i].frame, sizeof(cases[i].frame), i);
        expect_keys(cases[i].name, stick_expectation(cases[i].expected));
    }
}

static void test_streaming_and_recovery(void)
{
    static const uint8_t noise[] = {0x00,0x55,0x12,0xaa};
    static const uint8_t short_length[] = {0xaa,0x55,5};
    static const uint8_t long_length[] = {0xaa,0x55,41};
    static const uint8_t truncated[] = {0xaa,0x55,15,0xa0,1,0};
    uint8_t bad_checksum[sizeof(all_pressed)];
    bt_gamepad_stats_t s;
    bt_gamepad_test_reset();
    feed(noise, sizeof(noise), 0);
    feed(a_pressed, 4, 1); /* Includes AA AA 55 across the noise boundary. */
    expect_keys("partial frame does not publish", 0);
    feed(a_pressed + 4, 5, 8);
    expect_keys("second fragment does not publish", 0);
    feed(a_pressed + 9, 6, 15);
    expect_keys("three fragments complete", 1);
    feed(neutral, sizeof(neutral), 15);
    feed(all_pressed, sizeof(all_pressed), 15);
    expect_keys("back-to-back frames", 0x30f);
    s = stats();
    expect_u32("fragment and continuous frame count", s.data_frames, 3);
    expect_u32("all streaming bytes counted", s.bytes, 49);

    memcpy(bad_checksum, all_pressed, sizeof(bad_checksum));
    bad_checksum[14] = 0x7f; /* Correct value is 0x7e. */
    feed(bad_checksum, sizeof(bad_checksum), 16);
    feed(short_length, sizeof(short_length), 16);
    feed(long_length, sizeof(long_length), 16);
    expect_keys("bad checksum and lengths preserve snapshot", 0x30f);
    s = stats();
    expect_u32("checksum failure counted", s.checksum_errors, 1);
    expect_u32("short and oversized frames rejected", s.length_errors, 2);
    expect_u32("invalid frames not accepted", s.data_frames, 3);
    feed(neutral, sizeof(neutral), 17);
    expect_keys("recovered after malformed frames", 0);

    bt_gamepad_test_reset();
    feed(truncated, sizeof(truncated), 10);
    feed(a_pressed, sizeof(a_pressed), 31);
    expect_keys("gap abandons fragment and preserves current AA", 1);
    s = stats();
    expect_u32("gap recovery accepted one frame", s.data_frames, 1);
    expect_u32("gap recovery counted", s.resyncs, 1);

    bt_gamepad_test_reset();
    feed(a_pressed, 5, 0);
    feed(a_pressed + 5, sizeof(a_pressed) - 5, 20);
    expect_keys("20ms byte gap is accepted", 1);

    bt_gamepad_test_reset();
    feed(truncated, sizeof(truncated), 0);
    feed(a_pressed, sizeof(a_pressed), 1);
    expect_keys("header inside corrupt frame is recovered", 1);
    s = stats();
    expect_u32("corrupt prefix checksum error", s.checksum_errors, 1);
}

static void test_ignored_frames(void)
{
    static const uint8_t unknown_id[] = {
        0xaa,0x55,15,0x20,1,0,0,0,0,0,0,0,0,0,0x2f
    };
    static const uint8_t unknown_type[] = {0xaa,0x55,6,0xa0,2,0xa7};
    static const uint8_t short_data[] = {0xaa,0x55,6,0xa0,1,0xa6};
    static const uint8_t invalid_link[] = {0xaa,0x55,7,0xa0,0,1,0xa7};
    static const uint8_t invalid_dpad[] = {
        0xaa,0x55,15,0xa0,1,0,0,0,0,9,0,0,0,0,0xb8
    };
    static const uint8_t max_frame[40] = {
        0xaa,0x55,40,0x20,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x49
    };
    bt_gamepad_stats_t s;
    bt_gamepad_test_reset();
    feed(a_pressed, sizeof(a_pressed), 0);
    feed(unknown_id, sizeof(unknown_id), 1);
    feed(unknown_type, sizeof(unknown_type), 2);
    feed(short_data, sizeof(short_data), 3);
    feed(invalid_link, sizeof(invalid_link), 4);
    feed(invalid_dpad, sizeof(invalid_dpad), 5);
    feed(max_frame, sizeof(max_frame), 6);
    expect_keys("ignored frames preserve keys", 1);
    s = stats();
    expect_u32("six ignored frames", s.ignored_frames, 6);
    expect_u32("six-byte minimum and forty-byte maximum allowed", s.length_errors, 0);
    expect_u32("ignored data not counted", s.data_frames, 1);
    expect_u32("invalid link not counted", s.link_frames, 0);
}

static void test_link_and_uart_errors(void)
{
    bt_gamepad_stats_t s;
    bt_gamepad_test_reset();
    feed(all_pressed, sizeof(all_pressed), 1);
    feed(link_down, sizeof(link_down), 2);
    expect_keys("disconnect releases all keys", 0);
    s = stats();
    expect_u32("disconnect offline", s.connected, 0);
    expect_u32("disconnect invalidates snapshot", s.data_valid, 0);
    expect_u32("disconnect releases stick history", s.stick_keys, 0);
    feed(link_up, sizeof(link_up), 3);
    expect_keys("reconnect does not replay old keys", 0);
    s = stats();
    expect_u32("reconnect online", s.connected, 1);
    expect_u32("reconnect awaits data", s.data_valid, 0);
    expect_u32("both connection events counted", s.link_frames, 2);
    feed(a_pressed, sizeof(a_pressed), 4);
    expect_keys("data after reconnect", 1);
    feed(a_pressed, 5, 5);
    bt_gamepad_test_uart_error(0x02); /* UART overrun. */
    expect_keys("UART error releases keys", 0);
    s = stats();
    expect_u32("UART error offline", s.connected, 0);
    expect_u32("UART error invalidates snapshot", s.data_valid, 0);
    expect_u32("UART error count", s.uart_errors, 1);
    expect_u32("UART error keeps original LSR evidence", s.last_uart_error, 0x02);
    feed(a_pressed + 5, sizeof(a_pressed) - 5, 6);
    expect_keys("UART error discards partial packet", 0);
    feed(a_pressed, sizeof(a_pressed), 7);
    expect_keys("valid data restores UART input without link event", 1);
    s = stats();
    expect_u32("data restores online status", s.connected, 1);
}

static void test_timing_and_timeout(void)
{
    bt_gamepad_stats_t s;
    bt_gamepad_test_reset();
    feed(a_pressed, sizeof(a_pressed), 100);
    feed(a_pressed, sizeof(a_pressed), 112);
    feed(a_pressed, sizeof(a_pressed), 132);
    feed(a_pressed, sizeof(a_pressed), 137);
    bt_gamepad_test_set_time(140);
    s = stats();
    expect_u32("last data timestamp", s.last_data_ms, 137);
    expect_u32("last data interval", s.last_gap_ms, 5);
    expect_u32("maximum data interval", s.max_gap_ms, 20);
    expect_u32("data age", s.data_age_ms, 3);
    expect_u32("fresh data", s.stale, 0);

    bt_gamepad_test_reset();
    feed(a_pressed, sizeof(a_pressed), UINT32_MAX - 9u);
    feed(a_pressed, sizeof(a_pressed), 10);
    bt_gamepad_test_set_time(15);
    s = stats();
    expect_u32("frame interval across tick wrap", s.last_gap_ms, 20);
    expect_u32("maximum interval across tick wrap", s.max_gap_ms, 20);
    expect_u32("data age across tick wrap", s.data_age_ms, 5);
    expect_keys("tick wrap preserves valid input", 1);

    bt_gamepad_test_reset();
    feed(a_pressed, sizeof(a_pressed), UINT32_MAX - 9u);
#if BT_GAMEPAD_TIMEOUT_MS > 0
    bt_gamepad_test_set_time((UINT32_MAX - 9u) + (uint32_t)BT_GAMEPAD_TIMEOUT_MS - 1u);
    expect_keys("watchdog before boundary", 1);
    s = stats();
    expect_u32("watchdog fresh before boundary", s.stale, 0);
    bt_gamepad_test_set_time((UINT32_MAX - 9u) + (uint32_t)BT_GAMEPAD_TIMEOUT_MS);
    expect_keys("watchdog at boundary", 0);
    s = stats();
    expect_u32("watchdog stale at boundary", s.stale, 1);
    expect_u32("watchdog age across wrap", s.data_age_ms, BT_GAMEPAD_TIMEOUT_MS);
    feed(a_pressed, sizeof(a_pressed), 1000);
    expect_keys("fresh data resumes after timeout", 1);
    s = stats();
    expect_u32("fresh data clears stale", s.stale, 0);
#else
    bt_gamepad_test_set_time(1000000);
    expect_keys("default watchdog disabled for change-only reports", 1);
    s = stats();
    expect_u32("disabled watchdog never stale", s.stale, 0);
#endif
}

int main(void)
{
    test_key_contract_and_raw_data();
    test_button_and_direction_mapping();
    test_stick_hysteresis();
    test_streaming_and_recovery();
    test_ignored_frames();
    test_link_and_uart_errors();
    test_timing_and_timeout();
    if (failures) {
        printf("bt_gamepad regression: %u failure(s)\n", failures);
        return 1;
    }
    printf("bt_gamepad regression PASS: wire frames, mapping, hysteresis, recovery, link, UART, time; timeout=%u\n",
           (unsigned)BT_GAMEPAD_TIMEOUT_MS);
    return 0;
}
