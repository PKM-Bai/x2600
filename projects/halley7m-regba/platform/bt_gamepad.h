#ifndef REGBA_BT_GAMEPAD_H
#define REGBA_BT_GAMEPAD_H

#include <stdint.h>

/* Native GBA KEYINPUT bits; 1 means pressed at this interface. */
#define BT_GAMEPAD_KEY_A       0x001u
#define BT_GAMEPAD_KEY_B       0x002u
#define BT_GAMEPAD_KEY_SELECT  0x004u
#define BT_GAMEPAD_KEY_START   0x008u
#define BT_GAMEPAD_KEY_RIGHT   0x010u
#define BT_GAMEPAD_KEY_LEFT    0x020u
#define BT_GAMEPAD_KEY_UP      0x040u
#define BT_GAMEPAD_KEY_DOWN    0x080u
#define BT_GAMEPAD_KEY_R       0x100u
#define BT_GAMEPAD_KEY_L       0x200u
/* X-Protocol KEY2 bit4 is the physical Y button, reserved for the UI. */
#define BT_GAMEPAD_KEY2_Y      0x10u

/* X3S measured via SBTCOM1: right X=+127, up Y=+127, neutral=0.
 * No periodic neutral reports were observed; leave the watchdog disabled. */
#ifndef BT_GAMEPAD_X_RIGHT_SIGN
#define BT_GAMEPAD_X_RIGHT_SIGN 1
#endif
#ifndef BT_GAMEPAD_Y_UP_SIGN
#define BT_GAMEPAD_Y_UP_SIGN 1
#endif
#ifndef BT_GAMEPAD_TIMEOUT_MS
#define BT_GAMEPAD_TIMEOUT_MS 0
#endif
#if (BT_GAMEPAD_X_RIGHT_SIGN != 1 && BT_GAMEPAD_X_RIGHT_SIGN != -1) || \
    (BT_GAMEPAD_Y_UP_SIGN != 1 && BT_GAMEPAD_Y_UP_SIGN != -1)
#error Gamepad axis signs must be 1 or -1
#endif

typedef struct {
    uint32_t initialized, connected, data_valid;
    uint32_t bytes, data_frames, link_frames;
    uint32_t checksum_errors, length_errors, ignored_frames, resyncs;
    uint32_t uart_errors, uart_irqs, last_uart_error, last_uart_lsr;
    uint32_t last_data_ms, last_gap_ms, max_gap_ms;
    uint32_t data_age_ms, stale; /* age UINT32_MAX means no data received. */
    uint16_t keys, stick_keys;
    int8_t lx, ly, rx, ry;
    uint8_t dpad, key2, key3, rtrig, ltrig;
} bt_gamepad_stats_t;

/* UART2 RX only: PD12 FUNCTION0, J10 pin2; 115200 8N1, no flow control. */
int bt_gamepad_init(void);
uint16_t bt_gamepad_get_keys(void);
void bt_gamepad_get_stats(bt_gamepad_stats_t *stats);
void bt_gamepad_status(void);

#ifdef BT_GAMEPAD_HOST_TEST
void bt_gamepad_test_reset(void);
void bt_gamepad_test_feed(uint8_t byte, uint32_t now_ms);
void bt_gamepad_test_uart_error(uint32_t line_status);
void bt_gamepad_test_set_time(uint32_t now_ms);
#endif

#endif
