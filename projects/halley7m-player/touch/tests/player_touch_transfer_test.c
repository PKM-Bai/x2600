/* Exercise the production board transfer, not a copy of its state machine. */
#include "x2600_hal.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_register(size_t offset);
static void write_register(size_t offset, uint32_t value);
#define PLAYER_TOUCH_I2C_READ(reg) read_register(offsetof(I2C_TypeDef, reg))
#define PLAYER_TOUCH_I2C_WRITE(reg, value) write_register(offsetof(I2C_TypeDef, reg), (value))
#include "../player_touch_board.c"

I2C_TypeDef touch_test_i2c;
touch_test_cpm_t touch_test_cpm;
touch_test_gpio_t touch_test_gpio;

enum mode {
    NORMAL, PREEMPT_QUEUE, PREEMPT_FULL_RX, STOP_AFTER_LAST_RX,
    PREEMPT_CLOCK_CHECK, PREEMPT_ENABLE, STUCK_RX, STUCK_TX, NACK, OVERFLOW,
    MISSING_STOP, STUCK_ENABLE
};
static struct {
    enum mode mode;
    uint64_t now, due, queued_at;
    uint32_t commands[64];
    unsigned size, queued, received, enabled, ready, jumped, enable_waiting;
    unsigned completed_clock_reads, register_reads;
    uint32_t raw;
} peer;

static void update_hardware(void)
{
    if (peer.queued == peer.size + 2u && peer.now >= peer.due &&
        peer.mode != STUCK_RX && peer.mode != STUCK_TX) {
        peer.ready = 1;
        if (peer.mode != STOP_AFTER_LAST_RX && peer.mode != MISSING_STOP)
            peer.raw |= I2C_RAW_INTM_STAT_STOP_DET;
    }
    if (peer.mode == NACK && peer.queued) peer.raw |= I2C_RAW_INTM_STAT_TX_ABRT;
    if (peer.mode == OVERFLOW && peer.queued) peer.raw |= 1u << 1;
}

uint64_t platform_time_us(void)
{
    peer.now += 10;
    if (peer.mode == PREEMPT_ENABLE && peer.enable_waiting && !peer.jumped) {
        /* Enable state was pending at the preceding ENBST sample. It changes
         * while a higher-priority task preempts this deadline clock read. */
        peer.now += 12000;
        peer.jumped = 1;
        peer.enable_waiting = 0;
    }
    if (peer.queued == peer.size + 2u) {
        ++peer.completed_clock_reads;
        if (peer.mode == PREEMPT_CLOCK_CHECK && peer.completed_clock_reads == 2) {
            peer.now += 12000;
            peer.jumped = 1;
        }
    }
    return peer.now;
}

static uint32_t read_register(size_t offset)
{
    ++peer.register_reads;
    assert(peer.register_reads < 20000u); /* Independent bound on host test hangs. */
    if (offset == offsetof(I2C_TypeDef, I2C_ST) &&
        peer.mode == PREEMPT_FULL_RX && peer.queued == peer.size + 2u && !peer.jumped) {
        peer.now += 12000;
        peer.jumped = 1;
    }
    update_hardware();
    if (offset == offsetof(I2C_TypeDef, I2C_ENBST)) {
        if (peer.mode == PREEMPT_ENABLE && !peer.jumped) {
            peer.enable_waiting = 1;
            return !peer.enabled;
        }
        return peer.mode == STUCK_ENABLE ? !peer.enabled : peer.enabled;
    }
    if (offset == offsetof(I2C_TypeDef, I2C_RINTST)) return peer.raw;
    if (offset == offsetof(I2C_TypeDef, I2C_ABTSRC)) return 1;
    if (offset == offsetof(I2C_TypeDef, I2C_CINT)) {
        peer.raw = 0;
        return 0;
    }
    if (offset == offsetof(I2C_TypeDef, I2C_ST)) {
        uint32_t status = peer.mode == STUCK_TX ? 0u : I2C_ST_TFNF;
        if (peer.ready && peer.received < peer.size) status |= I2C_ST_RFNE;
        if (peer.queued && !peer.ready) status |= I2C_ST_MSTACT;
        return status;
    }
    if (offset == offsetof(I2C_TypeDef, I2C_DC)) {
        assert(peer.ready && peer.received < peer.size);
        uint32_t data = 0x80u + peer.received++;
        if (peer.received == peer.size && peer.mode == STOP_AFTER_LAST_RX) {
            /* STOP changes after the last data read. A loop-entry snapshot is
             * stale; task preemption can make an elapsed-time test expire. */
            peer.raw |= I2C_RAW_INTM_STAT_STOP_DET;
            peer.now += 12000;
            peer.jumped = 1;
        }
        return data;
    }
    return 0;
}

static void write_register(size_t offset, uint32_t value)
{
    if (offset == offsetof(I2C_TypeDef, I2C_ENABLE)) {
        peer.enabled = value;
        return;
    }
    if (offset == offsetof(I2C_TypeDef, I2C_TAR)) {
        assert(value == 0x14);
        return;
    }
    assert(offset == offsetof(I2C_TypeDef, I2C_DC));
    assert(peer.enabled && peer.queued < sizeof(peer.commands) / sizeof(peer.commands[0]));
    peer.commands[peer.queued++] = value;
    if (peer.mode == PREEMPT_QUEUE && peer.queued == 1) {
        peer.now += 12000;
        peer.jumped = 1;
    }
    if (peer.queued == peer.size + 2u) {
        peer.queued_at = peer.now;
        peer.due = peer.now + (peer.mode == STOP_AFTER_LAST_RX ? 0u : 300u);
    }
}

static void prepare_peer(enum mode mode, unsigned size)
{
    memset(&peer, 0, sizeof(peer));
    peer.mode = mode;
    peer.size = size;
    peer.due = UINT64_MAX;
}

static void expect_read(enum mode mode, unsigned size)
{
    uint8_t bytes[40];
    memset(bytes, 0, sizeof(bytes));
    prepare_peer(mode, size);
    assert(transfer(0x14, 0x814e, bytes, NULL, size) == 0);
    assert(peer.received == size && !peer.enabled);
    assert(peer.commands[0] == 0x81 && peer.commands[1] == 0x4e);
    for (unsigned i = 0; i < size; ++i) {
        assert(bytes[i] == (uint8_t)(0x80u + i));
        uint32_t expected = I2C_DC_CMD | (i == 0 ? I2C_DC_RESTART : 0u) |
            (i + 1u == size ? I2C_DC_STOP : 0u);
        assert(peer.commands[i + 2u] == expected);
    }
    if (mode != NORMAL) assert(peer.jumped && peer.now > TOUCH_TRANSFER_US);
}

int main(void)
{
    uint8_t bytes[40], clear = 0;
    expect_read(NORMAL, 1);
    expect_read(NORMAL, 40);
    expect_read(PREEMPT_QUEUE, 1);
    expect_read(PREEMPT_FULL_RX, 40);
    expect_read(STOP_AFTER_LAST_RX, 1);
    expect_read(PREEMPT_CLOCK_CHECK, 40);
    expect_read(PREEMPT_ENABLE, 1);

    prepare_peer(STUCK_RX, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -ETIMEDOUT);
    assert(peer.queued == 3 && peer.received == 0 && !peer.enabled);
    assert(peer.now - peer.queued_at >= TOUCH_TRANSFER_US && peer.now < TOUCH_TRANSFER_US + 100u);
    prepare_peer(STUCK_TX, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -ETIMEDOUT);
    assert(!peer.queued && !peer.enabled && peer.now < TOUCH_TRANSFER_US + 100u);
    prepare_peer(MISSING_STOP, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -ETIMEDOUT);
    assert(peer.received == 1 && !peer.enabled);
    prepare_peer(NACK, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -ENXIO && !peer.enabled);
    prepare_peer(OVERFLOW, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -EIO && !peer.enabled);
    prepare_peer(STUCK_ENABLE, 1);
    assert(transfer(0x14, 0x814e, bytes, NULL, 1) == -ETIMEDOUT);
    assert(!peer.queued && peer.now < TOUCH_ENABLE_US + 100u);

    prepare_peer(NORMAL, 1);
    assert(transfer(0x14, 0x814e, NULL, &clear, 1) == 0);
    assert(peer.commands[0] == 0x81 && peer.commands[1] == 0x4e &&
           peer.commands[2] == I2C_DC_STOP && !peer.enabled);
    assert(transfer(0x14, 0x814e, bytes, NULL, 41) == -EINVAL);
    puts("PASS GT911 production transfer: FIFO fill/drain, preemption, fresh STOP, final recheck, bounded TX/RX/enable, NACK/overflow");
    return 0;
}
