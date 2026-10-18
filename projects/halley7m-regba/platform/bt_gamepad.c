/* SBTCOM1 X-Protocol receiver. No Bluetooth stack or Linux input dependency. */
#include "bt_gamepad.h"
#include <string.h>

#ifndef BT_GAMEPAD_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <stdio.h>
#endif

enum { FRAME_MIN = 6, FRAME_MAX = 40, BYTE_GAP_MS = 20,
       UART_RX_BUDGET = 64, AXIS_PRESS = 48, AXIS_RELEASE = 32 };

static bt_gamepad_stats_t state;
static struct {
    uint8_t bytes[FRAME_MAX];
    unsigned count;
    uint32_t last_byte_ms;
} parser;

#ifdef BT_GAMEPAD_HOST_TEST
static uint32_t host_now;
static uint32_t now_ms(void) { return host_now; }
#else
static uint32_t now_ms(void) { return (uint32_t)(platform_time_us() / 1000u); }
#endif

static void release_keys(void)
{
    state.keys = state.stick_keys = 0;
    state.data_valid = 0;
}

static uint16_t axis_keys(int value, uint16_t previous,
                          uint16_t negative, uint16_t positive)
{
    if (value >= AXIS_PRESS) return positive;
    if (value <= -AXIS_PRESS) return negative;
    if ((previous & positive) && value > AXIS_RELEASE) return positive;
    if ((previous & negative) && value < -AXIS_RELEASE) return negative;
    return 0;
}

static void accept_frame(const uint8_t *frame, unsigned length, uint32_t now)
{
    static const uint16_t directions[9] = {
        0, BT_GAMEPAD_KEY_UP, BT_GAMEPAD_KEY_UP | BT_GAMEPAD_KEY_RIGHT,
        BT_GAMEPAD_KEY_RIGHT, BT_GAMEPAD_KEY_DOWN | BT_GAMEPAD_KEY_RIGHT,
        BT_GAMEPAD_KEY_DOWN, BT_GAMEPAD_KEY_DOWN | BT_GAMEPAD_KEY_LEFT,
        BT_GAMEPAD_KEY_LEFT, BT_GAMEPAD_KEY_UP | BT_GAMEPAD_KEY_LEFT
    };
    const uint8_t *data = frame + 5;
    uint16_t keys;
    if (frame[3] != 0xa0) {
        ++state.ignored_frames;
        return;
    }
    if (frame[4] == 0x00 && length == 7 &&
            (data[0] == 0x55 || data[0] == 0xff)) {
        ++state.link_frames;
        state.connected = data[0] == 0x55;
        release_keys(); /* A link event is not a new button snapshot. */
        return;
    }
    if (frame[4] != 0x01 || length != 15 || data[4] > 8) {
        ++state.ignored_frames;
        return;
    }
    if (state.data_valid) {
        state.last_gap_ms = now - state.last_data_ms;
        if (state.last_gap_ms > state.max_gap_ms)
            state.max_gap_ms = state.last_gap_ms;
    } else {
        state.last_gap_ms = 0;
    }
    state.last_data_ms = now;
    ++state.data_frames;
    state.connected = state.data_valid = 1;
    state.lx = (int8_t)data[0]; state.ly = (int8_t)data[1];
    state.rx = (int8_t)data[2]; state.ry = (int8_t)data[3];
    state.dpad = data[4]; state.key2 = data[5]; state.key3 = data[6];
    state.rtrig = data[7]; state.ltrig = data[8];
    state.stick_keys = axis_keys(state.lx * BT_GAMEPAD_X_RIGHT_SIGN,
                        state.stick_keys, BT_GAMEPAD_KEY_LEFT, BT_GAMEPAD_KEY_RIGHT) |
                       axis_keys(state.ly * BT_GAMEPAD_Y_UP_SIGN,
                        state.stick_keys, BT_GAMEPAD_KEY_DOWN, BT_GAMEPAD_KEY_UP);
    keys = state.dpad ? directions[state.dpad] : state.stick_keys;
    if (state.key2 & 0x01) keys |= BT_GAMEPAD_KEY_A;
    if (state.key2 & 0x02) keys |= BT_GAMEPAD_KEY_B;
    if (state.key2 & 0x40) keys |= BT_GAMEPAD_KEY_L;
    if (state.key2 & 0x80) keys |= BT_GAMEPAD_KEY_R;
    if (state.key3 & 0x04) keys |= BT_GAMEPAD_KEY_SELECT;
    if (state.key3 & 0x08) keys |= BT_GAMEPAD_KEY_START;
    state.keys = keys;
}

static void discard_bytes(unsigned count)
{
    parser.count -= count;
    memmove(parser.bytes, parser.bytes + count, parser.count);
}

/* Keep the next complete header or a final AA prefix after an error. This
 * recovers valid frames embedded after a truncated/corrupt-length frame.
 * The search and all buffer moves are bounded by FRAME_MAX. */
static void align_header(void)
{
    unsigned skip = 0;
    while (skip < parser.count) {
        if (parser.bytes[skip] == 0xaa &&
                (skip + 1 == parser.count || parser.bytes[skip + 1] == 0x55))
            break;
        ++skip;
    }
    if (skip) {
        discard_bytes(skip);
        ++state.resyncs;
    }
}

static void feed_byte(uint8_t byte, uint32_t now)
{
    ++state.bytes;
    if (parser.count && (uint32_t)(now - parser.last_byte_ms) > BYTE_GAP_MS) {
        parser.count = 0;
        ++state.resyncs;
    }
    parser.last_byte_ms = now;
    /* A complete frame is consumed immediately, so count is always <40. */
    parser.bytes[parser.count++] = byte;
    for (;;) {
        unsigned length, i;
        uint8_t checksum = 0;
        align_header();
        if (parser.count < 3) return;
        length = parser.bytes[2];
        if (length < FRAME_MIN || length > FRAME_MAX) {
            ++state.length_errors;
            discard_bytes(1);
            continue;
        }
        if (parser.count < length) return;
        for (i = 0; i + 1 < length; ++i) checksum += parser.bytes[i];
        if (checksum != parser.bytes[length - 1]) {
            ++state.checksum_errors;
            discard_bytes(1);
        } else {
            accept_frame(parser.bytes, length, now);
            discard_bytes(length);
        }
    }
}

static void uart_error(uint32_t line_status)
{
    ++state.uart_errors;
    state.last_uart_error = line_status;
    state.connected = 0;
    release_keys();
    parser.count = 0;
}

static void effective_state(bt_gamepad_stats_t *snapshot, uint32_t now)
{
    snapshot->data_age_ms = snapshot->data_frames ? now - snapshot->last_data_ms : UINT32_MAX;
    snapshot->stale = 0;
#if BT_GAMEPAD_TIMEOUT_MS > 0
    snapshot->stale = snapshot->data_valid &&
                      snapshot->data_age_ms >= BT_GAMEPAD_TIMEOUT_MS;
#endif
    if (!snapshot->connected || !snapshot->data_valid || snapshot->stale)
        snapshot->keys = 0;
}

#ifdef BT_GAMEPAD_HOST_TEST
void bt_gamepad_test_reset(void)
{
    memset(&state, 0, sizeof(state));
    memset(&parser, 0, sizeof(parser));
    host_now = 0;
    state.initialized = 1;
}
void bt_gamepad_test_set_time(uint32_t now) { host_now = now; }
void bt_gamepad_test_feed(uint8_t byte, uint32_t now)
{
    host_now = now;
    feed_byte(byte, now);
}
void bt_gamepad_test_uart_error(uint32_t line_status)
{
    uart_error(line_status);
}
void bt_gamepad_get_stats(bt_gamepad_stats_t *stats)
{
    *stats = state;
    effective_state(stats, now_ms());
}
#else
static void uart_irq(int irq, void *data)
{
    uint32_t now = now_ms();
    unsigned i;
    (void)irq; (void)data;
    ++state.uart_irqs;
    /* RDR/receive-timeout are cleared by draining RX. Reading LSR consumes
     * its error flags; on failure discard the FIFO and partial packet. */
    for (i = 0; i < UART_RX_BUDGET; ++i) {
        uint32_t line = UART2_Instance->ULSR;
        state.last_uart_lsr = line;
        if (line & (ULSR_OVER | ULSR_PARER | ULSR_FMER | ULSR_BI | ULSR_FIFOE)) {
            uart_error(line);
            UART2_Instance->UFCR = UFCR_UME | UFCR_FME | UFCR_RFRT;
            return;
        }
        if (!(line & ULSR_DRY)) return;
        feed_byte((uint8_t)UART2_Instance->URBR, now);
    }
}

int bt_gamepad_init(void)
{
    if (state.initialized) return 0;
    taskENTER_CRITICAL();
    memset(&state, 0, sizeof(state));
    memset(&parser, 0, sizeof(parser));
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_UART2);
    LL_GPIO_setPinMode(PD_Instance, 12, GPIO_MODE_FUNCTION0);
    /* The SDK pull helper compares physical *_BASE with a KSEG1 pointer on
     * MIPS and performs no write. Set only PD12, preserving other PD pins. */
    PD_Instance->PxPUS = 1u << 12;
    PD_Instance->PxPDC = 1u << 12; /* Pull up, no pull down; idle high. */
    /* Official 24MHz EXTAL table: divisor13, M16, ACR0 for 115200. */
    UART2_Instance->ULCR = 0x80;
    UART2_Instance->UDLHR = 0;
    UART2_Instance->UDLLR = 13;
    UART2_Instance->ULCR = 3;
    UART2_Instance->UIER = 0;
    UART2_Instance->UMR = 16;
    UART2_Instance->UACR = 0;
    UART2_Instance->UMCR = 0;
    UART2_Instance->ISR = 0;
    /* Lowest FIFO trigger (1 byte); no DMA, TX or modem interrupts. */
    UART2_Instance->UFCR = UFCR_UME | UFCR_FME | UFCR_RFRT | UFCR_TFRT;
    state.initialized = 1;
    ll_request_irq(IRQ_INTC1_UART2, uart_irq, NULL);
    UART2_Instance->UIER = UIER_RDRIE | UIER_RLSIE | UIER_RTOIE;
    taskEXIT_CRITICAL();
    return 0;
}

void bt_gamepad_get_stats(bt_gamepad_stats_t *stats)
{
    uint32_t now;
    taskENTER_CRITICAL();
    *stats = state;
    now = now_ms();
    taskEXIT_CRITICAL();
    effective_state(stats, now);
}

void bt_gamepad_status(void)
{
    bt_gamepad_stats_t s;
    bt_gamepad_get_stats(&s);
    printf("BTPAD initialized=%lu connected=%lu data_valid=%lu keys=%03x stale=%lu "
           "timeout_ms=%u x_right_sign=%d y_up_sign=%d axis_verified=1\n",
           (unsigned long)s.initialized, (unsigned long)s.connected,
           (unsigned long)s.data_valid, (unsigned)s.keys, (unsigned long)s.stale,
           (unsigned)BT_GAMEPAD_TIMEOUT_MS, BT_GAMEPAD_X_RIGHT_SIGN, BT_GAMEPAD_Y_UP_SIGN);
    printf("btpad_raw lx=%d ly=%d rx=%d ry=%d dpad=%u key2=%02x key3=%02x "
           "rtrig=%u ltrig=%u stick_keys=%03x\n",
           (int)s.lx, (int)s.ly, (int)s.rx, (int)s.ry, (unsigned)s.dpad,
           (unsigned)s.key2, (unsigned)s.key3, (unsigned)s.rtrig, (unsigned)s.ltrig,
           (unsigned)s.stick_keys);
    printf("btpad_rx bytes=%lu data_frames=%lu link_frames=%lu checksum_errors=%lu "
           "length_errors=%lu ignored_frames=%lu resyncs=%lu uart_errors=%lu "
           "last_gap_ms=%lu max_gap_ms=%lu data_age_ms=%lu\n",
           (unsigned long)s.bytes, (unsigned long)s.data_frames,
           (unsigned long)s.link_frames, (unsigned long)s.checksum_errors,
           (unsigned long)s.length_errors, (unsigned long)s.ignored_frames,
           (unsigned long)s.resyncs, (unsigned long)s.uart_errors,
           (unsigned long)s.last_gap_ms, (unsigned long)s.max_gap_ms,
           (unsigned long)s.data_age_ms);
    /* Do not read ULSR/UIIR here: those reads can clear IRQ/error evidence.
     * LSR snapshots come from the receiver itself; other registers below and
     * the GPIO input/mode are read-only observations of the current setup. */
    printf("btpad_hw uart_irqs=%lu last_lsr=%02lx last_error=%02lx "
           "pd12_level=%lu pd12_mode=%02lx pd12_pullup=%lu pd12_pulldown=%lu "
           "ier=%02lx lcr=%02lx mcr=%02lx "
           "umr=%lu uacr=%03lx uart2_gated=%lu intc_masked=%lu\n",
           (unsigned long)s.uart_irqs, (unsigned long)s.last_uart_lsr,
           (unsigned long)s.last_uart_error,
           (unsigned long)((LL_GPIO_getPinMaskLevel(PD_Instance) >> 12) & 1u),
           (unsigned long)LL_GPIO_getPinMode(PD_Instance, 12),
           (unsigned long)((PD_Instance->PxPU >> 12) & 1u),
           (unsigned long)((PD_Instance->PxPD >> 12) & 1u),
           UART2_Instance->UIER, UART2_Instance->ULCR, UART2_Instance->UMCR,
           UART2_Instance->UMR, UART2_Instance->UACR,
           (unsigned long)((CPM_Instance->CLKGR0 >> CPM_CLKID_UART2) & 1u),
           (unsigned long)((INTC0_Instance->ICMR1 >>
                          (IRQ_INTC1_UART2 - IRQ_INTC_BASE - 32)) & 1u));
}
#endif

uint16_t bt_gamepad_get_keys(void)
{
    bt_gamepad_stats_t snapshot;
    bt_gamepad_get_stats(&snapshot);
    return snapshot.keys;
}
