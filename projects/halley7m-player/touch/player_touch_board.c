/* HALLEY7-M screen touch connector. GPL-2.0-or-later.
 * Board mapping is verified from the shipped Linux DTB: I2C0 PC0/PC1
 * function 1, reset PC8, interrupt PC7. The x16xx SDK example uses different
 * pins and its 800x480 configuration must not be uploaded to this panel. */
#include "player_touch_core.h"
#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600_hal.h>
#include <errno.h>
#include <string.h>

#define TOUCH_BUS_HZ 100000u
#define TOUCH_TRANSFER_US 6000u
#define TOUCH_ENABLE_US 1000u
#define TOUCH_MAX_TRANSFER 40u

/* Register access remains direct in firmware. The production transfer loop is
 * also exercised with a FIFO/clock peer in the host regression. */
#ifndef PLAYER_TOUCH_I2C_READ
#define PLAYER_TOUCH_I2C_READ(reg) (I2C0_Instance->reg)
#endif
#ifndef PLAYER_TOUCH_I2C_WRITE
#define PLAYER_TOUCH_I2C_WRITE(reg, value) (I2C0_Instance->reg = (value))
#endif

static player_gt911_t controller;
static player_touch_stats_t published_stats;

static void publish_stats(void)
{
    /* The console may preempt the GUI owner during a protocol update. Publish
     * only a completed state; no bus operation runs in this critical section. */
    taskENTER_CRITICAL();
    published_stats = controller.stats;
    published_stats.last_raw = controller.sample;
    taskEXIT_CRITICAL();
}

static int set_enabled(unsigned enabled)
{
    uint64_t deadline = platform_time_us() + TOUCH_ENABLE_US;
    PLAYER_TOUCH_I2C_WRITE(I2C_ENABLE, enabled);
    while ((PLAYER_TOUCH_I2C_READ(I2C_ENBST) & 1u) != enabled) {
        if (platform_time_us() >= deadline)
            return (PLAYER_TOUCH_I2C_READ(I2C_ENBST) & 1u) == enabled ? 0 : -ETIMEDOUT;
    }
    return 0;
}

/* APB parent and divisors are read from the current boot profile; no PLL or
 * display/audio clock is changed. Formula follows the X2600 U-Boot clock code. */
static uint32_t apb_clock(void)
{
    uint32_t clock = CPM_Instance->CPCCR;
    uint32_t mux = (clock & CPCCR_SEL_H2PLL) >> CPCCR_SEL_H2PLL_Pos;
    uint32_t pll;
    if (mux == 1) pll = CPM_Instance->CPAPCR;
    else if (mux == 2) pll = CPM_Instance->CPMPCR;
    else return 0;
    uint32_t m = (pll >> 20) & 0xfffu, n = (pll >> 14) & 0x3fu;
    uint32_t od0 = (pll >> 8) & 7u, od1 = (pll >> 11) & 7u;
    if (!m || !n || !od0 || !od1) return 0;
    uint32_t parent = (uint32_t)((uint64_t)SYSCLK_EXTAL * m / n / od0 / od1);
    return parent / (((clock & CPCCR_PDIV) >> CPCCR_PDIV_Pos) + 1u);
}

static int bus_init(void)
{
    uint32_t rate = apb_clock();
    if (rate < TOUCH_BUS_HZ * 16u) return -ENOTSUP;
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_I2C0);
    LL_GPIO_setPinMode(PC_Instance, 0, GPIO_MODE_FUNCTION1);
    LL_GPIO_setPinMode(PC_Instance, 1, GPIO_MODE_FUNCTION1);
    int result = set_enabled(0);
    if (result) return result;
    /* Standard mode, master, slave disabled, repeated START enabled. Counts
     * and SDA setup/hold follow the SDK's polling driver, with bounded waits. */
    I2C0_Instance->I2C_CON = I2C_CON_SLVDIS | I2C_CON_RESTART |
                                  I2C_CON_SPEED_0 | I2C_CON_MD;
    I2C0_Instance->I2C_SHCNT = rate / (TOUCH_BUS_HZ * 2u) - 8u;
    I2C0_Instance->I2C_SLCNT = rate / (TOUCH_BUS_HZ * 2u) - 1u;
    uint32_t setup = rate / (TOUCH_BUS_HZ * 4u) - 1u;
    I2C0_Instance->I2C_SDASU = setup > 255u ? 255u : setup;
    I2C0_Instance->I2C_SDAHD = rate / (TOUCH_BUS_HZ * 4u);
    I2C0_Instance->I2C_FSPKLEN = 15;
    I2C0_Instance->I2C_INTM = 0;
    I2C0_Instance->I2C_DMACR = 0; /* Audio retains the only PDMA owner. */
    I2C0_Instance->I2C_RXTL = 0;
    I2C0_Instance->I2C_TXTL = 0;
    (void)I2C0_Instance->I2C_CINT;
    return 0;
}

/* This is one complete combined I2C transaction. Up to 42 commands fit the
 * controller's 64-entry FIFO. The final command requests STOP; read commands
 * begin with repeated START after the big-endian 16-bit register address.
 * No interrupt disabling, DMA, allocator, print or unbounded wait occurs. */
static int transfer(uint8_t address, uint16_t reg, uint8_t *read_data,
                    const uint8_t *write_data, size_t size)
{
    size_t received = 0, queued = 0;
    uint64_t deadline;
    int result, disabled;
    if (!size || size > TOUCH_MAX_TRANSFER || (!read_data && !write_data)) return -EINVAL;
    result = set_enabled(0);
    if (result) return result;
    (void)PLAYER_TOUCH_I2C_READ(I2C_CINT);
    PLAYER_TOUCH_I2C_WRITE(I2C_TAR, address);
    result = set_enabled(1);
    if (result) return result;
    deadline = platform_time_us() + TOUCH_TRANSFER_US;
    const size_t commands = size + 2u;
    while (queued < commands) {
        /* At most 42 commands fit in the 64-entry FIFO. Feed all available
         * space before checking elapsed time so the controller can finish
         * autonomously if the higher-priority media task preempts this task. */
        while (queued < commands && (PLAYER_TOUCH_I2C_READ(I2C_ST) & I2C_ST_TFNF)) {
            uint32_t command;
            if (!queued) command = reg >> 8;
            else if (queued == 1) command = reg & 255u;
            else if (read_data) command = I2C_DC_CMD | (queued == 2 ? I2C_DC_RESTART : 0u);
            else command = write_data[queued - 2u];
            if (queued + 1u == commands) command |= I2C_DC_STOP;
            PLAYER_TOUCH_I2C_WRITE(I2C_DC, command);
            ++queued;
        }
        uint32_t interrupt = PLAYER_TOUCH_I2C_READ(I2C_RINTST);
        if (interrupt & I2C_RAW_INTM_STAT_TX_ABRT) {
            result = (PLAYER_TOUCH_I2C_READ(I2C_ABTSRC) & 1u) ? -ENXIO : -EIO;
            goto finished;
        }
        if (interrupt & ((1u << 1) | (1u << 3))) { /* RX_OVER / TX_OVER */
            result = -EIO;
            goto finished;
        }
        if (queued < commands && platform_time_us() >= deadline) {
            result = -ETIMEDOUT;
            goto finished;
        }
    }
    /* Keep the device-completion budget at 6ms. Time spent suspended before
     * the last FIFO command was queued is not an I2C completion timeout. */
    deadline = platform_time_us() + TOUCH_TRANSFER_US;
    unsigned final_check = 0;
    for (;;) {
        /* Drain all received bytes before considering a timeout. A completed
         * 40-byte transfer can be waiting in RX FIFO when this task resumes. */
        while (read_data && received < size &&
               (PLAYER_TOUCH_I2C_READ(I2C_ST) & I2C_ST_RFNE))
            read_data[received++] = (uint8_t)PLAYER_TOUCH_I2C_READ(I2C_DC);
        uint32_t interrupt = PLAYER_TOUCH_I2C_READ(I2C_RINTST);
        if (interrupt & I2C_RAW_INTM_STAT_TX_ABRT) {
            result = (PLAYER_TOUCH_I2C_READ(I2C_ABTSRC) & 1u) ? -ENXIO : -EIO;
            break;
        }
        if (interrupt & ((1u << 1) | (1u << 3))) {
            result = -EIO;
            break;
        }
        /* STOP is sampled after RX service, not from a previous loop's state. */
        if ((!read_data || received == size) &&
            (interrupt & I2C_RAW_INTM_STAT_STOP_DET) &&
            !(PLAYER_TOUCH_I2C_READ(I2C_ST) & I2C_ST_MSTACT)) {
            result = 0;
            break;
        }
        if (final_check) {
            result = -ETIMEDOUT;
            break;
        }
        /* One bounded final drain and fresh status check closes the race
         * between the last sample and a preemption during the clock read. */
        if (platform_time_us() >= deadline) final_check = 1;
    }
finished:
    /* Disable also flushes FIFO after an abort/timeout. Never wait indefinitely
     * for an unplugged touch panel; the core backs off failed polls to 100 ms. */
    disabled = set_enabled(0);
    (void)PLAYER_TOUCH_I2C_READ(I2C_CINT);
    return result ? result : disabled;
}

static int bus_read(void *context, uint8_t address, uint16_t reg, uint8_t *data, size_t size)
{
    (void)context;
    /* The shipped panel configuration advertises ten contacts. Keep every
     * physical transfer <=40 bytes / 6ms even when its full report is 80 bytes.
     * The core only acknowledges after both contiguous halves are read. */
    if (!size || size > PLAYER_GT911_MAX_CONTACTS * 8u) return -EINVAL;
    while (size) {
        size_t chunk = size > TOUCH_MAX_TRANSFER ? TOUCH_MAX_TRANSFER : size;
        int result = transfer(address, reg, data, NULL, chunk);
        if (result) return result;
        reg = (uint16_t)(reg + chunk);
        data += chunk;
        size -= chunk;
    }
    return 0;
}

static int bus_write(void *context, uint8_t address, uint16_t reg,
                     const uint8_t *data, size_t size)
{
    (void)context;
    /* Runtime writes are limited to acknowledging the ready coordinate buffer.
     * Never send a new panel configuration or firmware as part of init/poll. */
    if (reg != PLAYER_GT911_STATUS || size != 1 || data[0]) return -EPERM;
    return transfer(address, reg, NULL, data, size);
}

int player_touch_init(void)
{
    memset(&controller, 0, sizeof(controller));
    int result = bus_init();
    if (result) {
        controller.stats.last_error = result;
        controller.stats.errors = 1;
        publish_stats();
        return result;
    }
    /* Goodix reset/address-select timing: RST low >10ms, INT high >100us,
     * RST high >5ms, INT low 50ms, then input. Select verified board addr14. */
    LL_GPIO_setPinMode(PC_Instance, 8, GPIO_MODE_OUTPUT0);
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    LL_GPIO_setPinMode(PC_Instance, 7, GPIO_MODE_OUTPUT1);
    vTaskDelay(pdMS_TO_TICKS(1) + 1);
    LL_GPIO_setPinMode(PC_Instance, 8, GPIO_MODE_OUTPUT1);
    vTaskDelay(pdMS_TO_TICKS(6) + 1);
    LL_GPIO_setPinMode(PC_Instance, 8, GPIO_MODE_INPUT);
    LL_GPIO_setPinMode(PC_Instance, 7, GPIO_MODE_OUTPUT0);
    vTaskDelay(pdMS_TO_TICKS(50) + 1);
    LL_GPIO_setPinMode(PC_Instance, 7, GPIO_MODE_INPUT);
    const player_touch_bus_t bus = {NULL, bus_read, bus_write};
    result = player_gt911_init(&controller, &bus);
    publish_stats();
    return result;
}

int player_touch_poll(uint32_t now_ms, player_touch_sample_t *sample)
{
    int result = player_gt911_poll(&controller, now_ms, sample);
    publish_stats();
    return result;
}

void player_touch_get_stats(player_touch_stats_t *stats)
{
    if (!stats) return;
    taskENTER_CRITICAL();
    *stats = published_stats;
    taskEXIT_CRITICAL();
}
