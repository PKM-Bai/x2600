/* HALLEY7-M J9: PB3 SCK, PB2 MOSI, PB1 MISO, PB0 GPIO CS.
 * No SD commands which erase/format media are implemented. */
#include "sdspi.h"
#include <errno.h>
#include <string.h>

uint8_t sdspi_crc7(const uint8_t *data, size_t size)
{
    uint8_t crc = 0;
    while (size--) {
        uint8_t value = *data++;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if ((value ^ crc) & 0x80) crc ^= 0x09;
            value <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1);
}

uint16_t sdspi_crc16(const uint8_t *data, size_t size)
{
    static const uint16_t table[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
        0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef
    };
    uint16_t crc = 0;
    while (size--) {
        uint8_t value = *data++;
        crc = (uint16_t)((crc << 4) ^ table[(crc >> 12) ^ (value >> 4)]);
        crc = (uint16_t)((crc << 4) ^ table[(crc >> 12) ^ (value & 15)]);
    }
    return crc;
}

int sdspi_decode_csd(const uint8_t csd[16], uint32_t *sectors)
{
    uint64_t count;
    if ((csd[0] >> 6) == 1) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3f) << 16) |
                          ((uint32_t)csd[8] << 8) | csd[9];
        count = ((uint64_t)c_size + 1) * 1024;
    } else if ((csd[0] >> 6) == 0) {
        unsigned block_bits = csd[5] & 15;
        unsigned mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
        uint32_t c_size = ((uint32_t)(csd[6] & 3) << 10) |
                          ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        if (block_bits > 11) return -EINVAL;
        count = ((uint64_t)c_size + 1) << (mult + 2 + block_bits);
        count >>= 9;
    } else return -ENOTSUP;
    if (!count || count > UINT32_MAX) return -EOVERFLOW;
    *sectors = (uint32_t)count;
    return 0;
}

#ifndef SDSPI_HOST_TEST
#include <x2600_hal.h>
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include <stdio.h>

static SPI_HandleTypeDef spi;
static uint32_t sectors;
static int initialized, block_addressed, io_error;

/* The filesystem task owns this trace along with the bus. Print only after CS
 * is high, so diagnostics never insert UART delays into an SD transaction. */
typedef struct {
    const char *operation, *stage;
    uint32_t sector, count, done, command, argument;
    unsigned r1, token, r2, ready, crc_card, crc_calculated;
    int hal_result;
    uint32_t hal_state, hal_error, controller_status;
    unsigned xfer_failure, tx_left, rx_left, chunk;
    uint64_t hal_us;
    unsigned precleanup;
    uint32_t pre_cr0, pre_cr1, pre_status;
    unsigned r1_polls, ff_tokens, illegal_token, extra_count;
    uint64_t token_exchange_us;
    uint32_t token_cr0, token_cr1, token_status, token_cs, token_hal_state, token_hal_error;
    uint8_t extra_data[16];
    int extra_result, extra_hal_result;
    unsigned initializing, init_valid;
    uint8_t init_r7[4], init_ocr[4], init_csd[16];
    unsigned command_sent, stop_attempted, stop_r1, stop_ready;
    int stop_result, stop_io_error;
} sdspi_trace_t;
static sdspi_trace_t trace;
static uint32_t failure_count;

/* Polling SSI is used only by the filesystem task after the scheduler starts.
 * Its 1 kHz tick avoids a 64-bit OST division on every transferred byte. */
uint32_t regba_ssi_poll_tick(void)
{
    return (uint32_t)xTaskGetTickCount();
}

/* Called only by the generated polling HAL timeout path, before it changes
 * receive control, clears errors, or flushes either FIFO. No UART/bus recovery.
 */
void regba_ssi_timeout_snapshot(SPI_HandleTypeDef *hspi)
{
    if (hspi != &spi) return;
    trace.pre_status = READ_REG(hspi->Instance->SSISR);
    trace.pre_cr0 = READ_REG(hspi->Instance->SSICR0);
    trace.pre_cr1 = READ_REG(hspi->Instance->SSICR1);
    trace.tx_left = hspi->TxXferCount;
    trace.rx_left = hspi->RxXferCount;
    trace.precleanup = 1;
}

static void trace_begin(const char *operation, uint32_t sector, uint32_t count)
{
    memset(&trace, 0, sizeof(trace));
    trace.operation = operation;
    trace.stage = "bounds";
    trace.sector = sector;
    trace.count = count;
    trace.command = trace.r1 = trace.token = trace.r2 = trace.ready = 0xff;
}

static int trace_finish(int result)
{
    if (result < 0) {
        if (trace.illegal_token) {
            /* Keep the successful exchange which returned the illegal token
             * separate from optional diagnostic reads and bus cleanup. */
            trace.hal_result = HAL_OK;
            trace.hal_state = trace.token_hal_state;
            trace.hal_error = trace.token_hal_error;
            trace.controller_status = trace.token_status;
        } else if (!io_error && spi.Instance) {
            trace.hal_state = spi.State;
            trace.hal_error = spi.ErrorCode;
            trace.controller_status = READ_REG(spi.Instance->SSISR);
        }
        if (trace.illegal_token) {
            /* Print this before SD_IO: host monitors may stop at result=-5. */
            printf("SD_BAD_TOKEN r1_polls=%u ff_tokens=%u exchange_us=%llu "
                   "cr0=%08lx cr1=%08lx ssisr=%08lx cs=%lu "
                   "diag_result=%d diag_hal=%d cleanup_io=%d bytes=%u data=",
                   trace.r1_polls, trace.ff_tokens,
                   (unsigned long long)trace.token_exchange_us,
                   (unsigned long)trace.token_cr0, (unsigned long)trace.token_cr1,
                   (unsigned long)trace.token_status, (unsigned long)trace.token_cs,
                   trace.extra_result, trace.extra_hal_result, io_error,
                   trace.extra_count);
            for (unsigned n = 0; n < trace.extra_count; n++)
                printf("%02x", trace.extra_data[n]);
            printf("\n");
        }
        if (trace.initializing) {
            const char *names[] = { "r7", "ocr", "csd" };
            const uint8_t *values[] = { trace.init_r7, trace.init_ocr, trace.init_csd };
            const unsigned sizes[] = { 4, 4, 16 };
            printf("SD_INIT");
            for (unsigned reg = 0; reg < 3; reg++) {
                printf(" %s=", names[reg]);
                if (trace.init_valid & (1u << reg)) {
                    for (unsigned n = 0; n < sizes[reg]; n++)
                        printf("%02x", values[reg][n]);
                } else printf("unknown");
            }
            printf("\n");
        }
        printf("SD_IO failure=%lu op=%s stage=%s sector=%lu count=%lu done=%lu "
               "cmd=%lu arg=%08lx r1=%02x token=%02x r2=%02x ready=%02x "
               "crc_card=%04x crc_calc=%04x result=%d io_error=%d "
               "hal=%d state=%lu error=%08lx ssisr=%08lx",
               (unsigned long)++failure_count, trace.operation, trace.stage,
               (unsigned long)trace.sector, (unsigned long)trace.count,
               (unsigned long)trace.done, (unsigned long)trace.command,
               (unsigned long)trace.argument, trace.r1, trace.token, trace.r2,
               trace.ready, trace.crc_card, trace.crc_calculated, result,
               trace.illegal_token ? 0 : io_error,
               trace.hal_result, (unsigned long)trace.hal_state,
               (unsigned long)trace.hal_error,
               (unsigned long)trace.controller_status);
        if (trace.xfer_failure && !trace.illegal_token)
            printf(" tx_left=%u rx_left=%u chunk=%u hal_us=%llu",
                   trace.tx_left, trace.rx_left, trace.chunk,
                   (unsigned long long)trace.hal_us);
        if (trace.precleanup && !trace.illegal_token)
            printf(" pre_cr0=%08lx pre_cr1=%08lx pre_ssisr=%08lx",
                   (unsigned long)trace.pre_cr0, (unsigned long)trace.pre_cr1,
                   (unsigned long)trace.pre_status);
        if (trace.stop_attempted)
            printf(" stop_result=%d stop_r1=%02x stop_ready=%02x stop_io=%d",
                   trace.stop_result, trace.stop_r1, trace.stop_ready,
                   trace.stop_io_error);
        printf("\n");
    }
    return result;
}

static int bus_idle(void)
{
    uint64_t deadline = platform_time_us() + 100000;
    for (;;) {
        uint32_t status = READ_REG(spi.Instance->SSISR);
        if ((status & SSISR_END_Msk) && !(status & SSISR_BUSY_Msk)) return 0;
        if (platform_time_us() >= deadline) return -ETIMEDOUT;
    }
}

static void bus_recover(void)
{
    LL_GPIO_setOutputPin(PB_Instance, 0);
    __HAL_SPI_DISABLE(&spi);
    WRITE_REG(spi.Instance->SSICR0, 0);
    spi.State = HAL_SPI_STATE_RESET;
    HAL_SPI_Init(&spi); /* Polling path: no DMA handle is supplied/started. */
}

static int exchange(const uint8_t *tx, uint8_t *rx, size_t size)
{
    /* The derived polling HAL bounds outstanding TX/RX to 64 characters, so
     * a full sector can share one setup/END sequence even across preemption. */
    uint8_t fill[512], discard[512];
    if (!tx) memset(fill, 0xff, size > sizeof(fill) ? sizeof(fill) : size);
    while (size) {
        uint16_t chunk = size > sizeof(fill) ? sizeof(fill) : (uint16_t)size;
        uint64_t hal_started_us = platform_time_us();
        HAL_StatusTypeDef hal_result = HAL_SPI_TransmitReceive(
            &spi, (uint8_t *)(tx ? tx : fill), rx ? rx : discard, chunk, 100);
        uint64_t hal_us = platform_time_us() - hal_started_us;
        if (hal_result != HAL_OK || bus_idle() < 0) {
            trace.hal_result = hal_result;
            trace.hal_state = spi.State;
            trace.hal_error = spi.ErrorCode;
            trace.controller_status = READ_REG(spi.Instance->SSISR);
            trace.xfer_failure = 1;
            if (!trace.precleanup) {
                trace.tx_left = spi.TxXferCount;
                trace.rx_left = spi.RxXferCount;
            }
            trace.chunk = chunk;
            trace.hal_us = hal_us;
            io_error = -ETIMEDOUT;
            bus_recover(); /* HAL timeout otherwise leaves State busy. */
            return io_error;
        }
        if (tx) tx += chunk;
        if (rx) rx += chunk;
        size -= chunk;
    }
    return 0;
}

static uint8_t read_byte(void)
{
    uint8_t byte = 0xff;
    if (io_error) return 0xff;
    exchange(NULL, &byte, 1);
    return byte;
}

static void deselect(void)
{
    /* Every successful exchange drains RX, so the final bit has completed. */
    LL_GPIO_setOutputPin(PB_Instance, 0);
    if (!io_error) exchange(NULL, NULL, 1);
}

static int ready(uint32_t timeout_ms)
{
    uint64_t deadline = platform_time_us() + (uint64_t)timeout_ms * 1000;
    unsigned polls = 0;
    do {
        trace.ready = read_byte();
        if (trace.ready == 0xff) return io_error;
        if (io_error) return io_error;
        if (++polls % 64 == 0) vTaskDelay(1);
    } while (platform_time_us() < deadline);
    return -ETIMEDOUT;
}

static int select_card(void)
{
    LL_GPIO_clearOutputPin(PB_Instance, 0);
    read_byte();
    return ready(500);
}

/* Returns the raw R1 byte or a negative errno. Leaves CS low on success. */
static int command(unsigned cmd, uint32_t arg)
{
    uint8_t packet[6] = { (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
        (uint8_t)(arg >> 8), (uint8_t)arg, 0 };
    trace.command = cmd;
    trace.argument = arg;
    trace.r1 = trace.token = trace.r2 = trace.ready = 0xff;
    trace.r1_polls = 0;
    trace.command_sent = 0;
    trace.crc_card = trace.crc_calculated = 0;
    if (cmd != 12) {
        trace.stage = "command-deselect";
        deselect();
        if (io_error) return io_error;
        trace.stage = "command-select";
        int result = select_card();
        if (result < 0) return result;
    } else {
        /* CMD12 interrupts CMD18 without releasing CS or waiting for ready.
         * A preceding HAL recovery may have raised CS; reassert it directly. */
        LL_GPIO_clearOutputPin(PB_Instance, 0);
    }
    packet[5] = sdspi_crc7(packet, 5);
    trace.stage = "command-send";
    trace.command_sent = 1;
    if (exchange(packet, NULL, sizeof(packet)) < 0) return io_error;
    if (cmd == 12) {
        trace.stage = "stop-stuff";
        (void)read_byte(); /* The byte immediately after CMD12 is not R1. */
        if (io_error) return io_error;
    }
    trace.stage = "command-r1";
    for (unsigned n = 0; n < 10; n++) {
        uint8_t response = read_byte();
        trace.r1 = response;
        trace.r1_polls = n + 1;
        if (io_error) return io_error;
        if (!(response & 0x80)) return response;
    }
    return -ETIMEDOUT;
}

static void capture_illegal_token(unsigned ff_tokens, uint64_t exchange_us)
{
    trace.illegal_token = 1;
    trace.ff_tokens = ff_tokens;
    trace.token_exchange_us = exchange_us;
    trace.token_cr0 = READ_REG(spi.Instance->SSICR0);
    trace.token_cr1 = READ_REG(spi.Instance->SSICR1);
    trace.token_status = READ_REG(spi.Instance->SSISR);
    trace.token_cs = LL_GPIO_getPinMaskLevel(PB_Instance) & 1u;
    trace.token_hal_state = spi.State;
    trace.token_hal_error = spi.ErrorCode;
    trace.extra_count = 0;
    trace.extra_result = 0;
    trace.extra_hal_result = HAL_OK;

    /* Inspect only an already failed read transaction, before deselecting.
     * These bytes never satisfy the requested read or trigger a retry. */
    for (unsigned n = 0; n < sizeof(trace.extra_data); n++) {
        uint8_t byte = read_byte();
        if (io_error) break;
        trace.extra_data[trace.extra_count++] = byte;
    }
    trace.extra_result = io_error;
    trace.extra_hal_result = trace.hal_result;
    /* A diagnostic timeout must not replace the original illegal-token EIO.
     * exchange() has already recovered the controller if it timed out. */
    io_error = 0;
}

static int receive_data(uint8_t *buffer, size_t size)
{
    uint64_t deadline = platform_time_us() + 300000;
    uint8_t token;
    unsigned polls = 0;
    uint64_t exchange_us;
    trace.stage = "read-token";
    do {
        uint64_t started = platform_time_us();
        token = read_byte();
        exchange_us = platform_time_us() - started;
        trace.token = token;
        if (io_error) return io_error;
        if (token != 0xff) break;
        if (++polls % 64 == 0) vTaskDelay(1);
    } while (platform_time_us() < deadline);
    if (token != 0xfe) {
        if (token != 0xff && (token & 0xf0))
            capture_illegal_token(polls, exchange_us);
        return token == 0xff ? -ETIMEDOUT : -EIO;
    }
    trace.stage = "read-data";
    if (exchange(NULL, buffer, size) < 0) return io_error;
    trace.stage = "read-crc";
    uint16_t crc = (uint16_t)read_byte() << 8;
    crc |= read_byte();
    if (io_error) return io_error;
    trace.crc_card = crc;
    trace.crc_calculated = sdspi_crc16(buffer, size);
    return trace.crc_calculated == crc ? 0 : -EILSEQ;
}

static int stop_read(int original_result)
{
    sdspi_trace_t original_trace = trace;
    int original_io = io_error;
    /* exchange has already reset SSI after a HAL fault. Clear the sticky
     * result only for this stop attempt; never retry the failed data block. */
    io_error = 0;
    int result = command(12, 0);
    if (result >= 0) {
        int r1 = result;
        trace.stage = "stop-ready";
        result = ready(500); /* CMD12 returns R1b, including optional busy. */
        if (r1 != 0) result = -EIO;
    }
    unsigned stop_r1 = trace.r1, stop_ready = trace.ready;
    deselect();
    if (result == 0 && io_error) result = io_error;
    int stop_io = io_error;
    if (original_result < 0) {
        trace = original_trace;
        io_error = original_io;
    }
    trace.stop_attempted = 1;
    trace.stop_result = result;
    trace.stop_r1 = stop_r1;
    trace.stop_ready = stop_ready;
    trace.stop_io_error = stop_io;
    return original_result < 0 ? original_result : result;
}

static int set_speed(unsigned divisor)
{
    int result = bus_idle();
    if (result < 0) return result;
    __HAL_SPI_DISABLE(&spi);
    WRITE_REG(spi.Instance->SSIGR, divisor - 1);
    __HAL_SPI_ENABLE(&spi);
    return 0;
}

static int set_data_speed(const uint8_t csd[16])
{
    static const uint32_t units[] = { 100000, 1000000, 10000000, 100000000 };
    static const uint8_t tenths[] = { 0, 10, 12, 13, 15, 20, 25, 30,
                                    35, 40, 45, 50, 55, 60, 70, 80 };
    unsigned encoded = csd[3], unit = encoded & 7, value = (encoded >> 3) & 15;
    if ((encoded & 0x80) || unit >= 4 || !value) return -EINVAL;
    uint32_t card_hz = units[unit] / 10u * tenths[value];
    /* SSI input / 2 is 56.25MHz. Aim for 18.75MHz, bounded by TRAN_SPEED. */
    unsigned divisor = (56250000u + card_hz - 1u) / card_hz;
    if (divisor < 3) divisor = 3;
    if (divisor > 256) return -ENOTSUP; /* SSIGR.CGV is eight bits. */
    int result = set_speed(divisor);
    if (result == 0)
        printf("SD_SPI data_hz=%lu card_max_hz=%lu tran_speed=%02x divisor=%u\n",
               (unsigned long)(56250000u / divisor), (unsigned long)card_hz,
               encoded, divisor);
    return result;
}

int sdspi_initialize(void)
{
    /* MPLL 1.8GHz / CGU 16 / (2 * (SSIGR + 1)). Initialize at
     * SSIGR=149 (375kHz), then SSIGR=2 (18.75MHz) or the CSD speed limit.
     * SSI1 / MSC1 pins are never touched. */
    LL_CPM_CGU_ConfigTypeDef clock = {
        .PLLMux = SSICDR_MPLL, .Div = 15, .Config = 0
    };
    trace_begin("initialize", 0, 0);
    trace.initializing = 1;
    uint8_t *r7 = trace.init_r7, *ocr = trace.init_ocr, *csd = trace.init_csd;
    int result;
    initialized = block_addressed = io_error = 0;
    sectors = 0;
    memset(&spi, 0, sizeof(spi));
    trace.stage = "clock";
    if (CPM_CGU_SSI_Start(CPM_Instance, &clock) != HAL_OK) return trace_finish(-EIO);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_SSI0);
    LL_GPIO_setPinMode(PB_Instance, 0, GPIO_MODE_OUTPUT1);
    for (unsigned pin = 1; pin <= 3; pin++)
        LL_GPIO_setPinMode(PB_Instance, pin, GPIO_MODE_FUNCTION0);
    /* Card DO must stay high when it is not driving the shared bus. */
    LL_GPIO_setPinPull(PB_Instance, 1, 1); /* LL API: 1 = pull-up. */
    spi.Instance = SSI0_Instance;
    spi.Init.DataSize = SPI_DATASIZE_8BIT;
    spi.Init.TransferEndian = SPI_T_MSB_BYTE_MSB_BIT;
    spi.Init.ReceiveEndian = SPI_R_MSB_BYTE_MSB_BIT;
    spi.Init.FrameVaildLevel = SPI_CE0_LOW_CE1_LOW;
    spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    spi.Init.CLKPhase = SPI_PHASE_1EDGE;
    spi.Init.LoopMode = SPI_LOOP_DISABLE;
    WRITE_REG(spi.Instance->SSICR0, 0);
    WRITE_REG(spi.Instance->SSIGR, 149);
    trace.stage = "spi-init";
    HAL_StatusTypeDef hal_result = HAL_SPI_Init(&spi);
    if (hal_result != HAL_OK) {
        trace.hal_result = hal_result;
        return trace_finish(-EIO);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    trace.stage = "power-up-clocks";
    if (exchange(NULL, NULL, 10) < 0) return trace_finish(io_error); /* >=74 clocks, CS high */
    result = command(0, 0);
    if (result != 1) { result = result < 0 ? result : -ENODEV; goto finish; }
    result = command(8, 0x1aa);
    if (result != 1) { result = result < 0 ? result : -ENOTSUP; goto finish; }
    trace.stage = "read-r7";
    if (exchange(NULL, r7, 4) < 0) { result = io_error; goto finish; }
    trace.init_valid |= 1u;
    trace.stage = "check-r7";
    if (r7[2] != 1 || r7[3] != 0xaa) { result = -EINVAL; goto finish; }
    uint64_t deadline = platform_time_us() + 2000000;
    do {
        result = command(55, 0);
        if (result < 0 || result > 1) { result = result < 0 ? result : -EIO; goto finish; }
        result = command(41, 1u << 30);
        if (result == 0) break;
        if (result != 1) { result = result < 0 ? result : -EIO; goto finish; }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (platform_time_us() < deadline);
    if (result != 0) { trace.stage = "acmd41-ready"; result = -ETIMEDOUT; goto finish; }
    result = command(58, 0);
    if (result != 0) { result = result < 0 ? result : -EIO; goto finish; }
    trace.stage = "read-ocr";
    if (exchange(NULL, ocr, 4) < 0) { result = io_error; goto finish; }
    trace.init_valid |= 2u;
    trace.stage = "check-ocr";
    if (!(ocr[0] & 0x80) || !(ocr[1] & 0x30)) { result = -EINVAL; goto finish; }
    block_addressed = !!(ocr[0] & 0x40);
    if (!block_addressed) {
        result = command(16, 512);
        if (result) { result = result < 0 ? result : -EIO; goto finish; }
    }
    result = command(9, 0);
    if (result) { result = result < 0 ? result : -EIO; goto finish; }
    result = receive_data(csd, 16);
    if (result == 0 || result == -EILSEQ) trace.init_valid |= 4u;
    if (result < 0) {
        if (!strcmp(trace.stage, "read-token")) trace.stage = "csd-token";
        else if (!strcmp(trace.stage, "read-data")) trace.stage = "csd-data";
        else if (!strcmp(trace.stage, "read-crc")) trace.stage = "csd-crc";
        goto finish;
    }
    trace.stage = "csd-decode";
    result = sdspi_decode_csd(csd, &sectors);
finish:
    deselect();
    if (trace.illegal_token) return trace_finish(-EIO);
    if (io_error) return trace_finish(io_error);
    if (result == 0) {
        trace.stage = "speed";
        result = set_data_speed(csd);
    }
    if (result == 0) initialized = 1;
    return trace_finish(result);
}

int sdspi_is_initialized(void) { return initialized; }
uint32_t sdspi_sector_count(void) { return sectors; }

/* This two-command diagnostic deliberately bypasses SSI and its HAL. Normal
 * mounts always restore the SSI pinmux and reinitialize the controller. */
static uint8_t probe_exchange(uint8_t tx)
{
    uint8_t rx = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        if (tx & 0x80) LL_GPIO_setOutputPin(PB_Instance, 2);
        else LL_GPIO_clearOutputPin(PB_Instance, 2);
        HAL_uDelay(5);
        LL_GPIO_setOutputPin(PB_Instance, 3);
        HAL_uDelay(5);
        rx = (uint8_t)((rx << 1) |
             ((LL_GPIO_getPinMaskLevel(PB_Instance) >> 1) & 1u));
        LL_GPIO_clearOutputPin(PB_Instance, 3);
        tx <<= 1;
    }
    return rx;
}

static void probe_deselect(void)
{
    LL_GPIO_setOutputPin(PB_Instance, 0);
    probe_exchange(0xff);
}

static uint8_t probe_command(unsigned cmd, uint32_t arg)
{
    uint8_t packet[6] = { (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24),
        (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0 };
    packet[5] = sdspi_crc7(packet, 5);
    LL_GPIO_clearOutputPin(PB_Instance, 0);
    probe_exchange(0xff);
    for (unsigned n = 0; n < sizeof(packet); n++) probe_exchange(packet[n]);
    for (unsigned n = 0; n < 10; n++) {
        uint8_t response = probe_exchange(0xff);
        if (!(response & 0x80)) return response;
    }
    return 0xff;
}

int sdspi_gpio_probe(void)
{
    /* The filesystem RPC rejects mounted volumes and all open file slots. */
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_SSI0);
    CLEAR_BIT(SSI0_Instance->SSICR0, SSICR0_SSIE_Msk);
    LL_GPIO_setPinMode(PB_Instance, 0, GPIO_MODE_OUTPUT1);
    LL_GPIO_setPinMode(PB_Instance, 3, GPIO_MODE_OUTPUT0);
    LL_GPIO_setPinMode(PB_Instance, 2, GPIO_MODE_OUTPUT1);
    LL_GPIO_setPinMode(PB_Instance, 1, GPIO_MODE_INPUT);
    LL_GPIO_setPinPull(PB_Instance, 1, 1);
    initialized = block_addressed = io_error = 0;
    sectors = 0;
    unsigned passed = 0;
    printf("TF_PROBE transport=gpio mode=0 half_period_us=5 trials=8 commands=CMD0,CMD8\n");
    for (unsigned trial = 1; trial <= 8; trial++) {
        /* Each trial restarts from CMD0, not a retry inside normal transport. */
        LL_GPIO_setOutputPin(PB_Instance, 0);
        for (unsigned n = 0; n < 10; n++) probe_exchange(0xff);
        uint8_t r1_0 = probe_command(0, 0), r1_8 = 0xff, r7[4] = {0};
        int tried_cmd8 = 0, got_r7 = 0;
        probe_deselect();
        if (r1_0 == 1) {
            tried_cmd8 = 1;
            r1_8 = probe_command(8, 0x1aa);
            if (r1_8 == 1) {
                for (unsigned n = 0; n < sizeof(r7); n++) r7[n] = probe_exchange(0xff);
                got_r7 = 1;
            }
            probe_deselect();
        }
        unsigned pass = r1_0 == 1 && r1_8 == 1 && got_r7 && r7[2] == 1 && r7[3] == 0xaa;
        passed += pass;
        /* UART output is restricted to CS-high boundaries. */
        printf("TF_PROBE trial=%u cmd0_r1=%02x cmd8_r1=", trial, r1_0);
        if (tried_cmd8) printf("%02x", r1_8); else printf("unknown");
        printf(" r7=");
        if (got_r7) for (unsigned n = 0; n < sizeof(r7); n++) printf("%02x", r7[n]);
        else printf("unknown");
        printf(" pass=%u\n", pass);
    }
    LL_GPIO_setOutputPin(PB_Instance, 0);
    LL_GPIO_clearOutputPin(PB_Instance, 3);
    LL_GPIO_setOutputPin(PB_Instance, 2);
    initialized = 0;
    printf("TF_PROBE done trials=8 passed=%u initialized=0\n", passed);
    return passed == 8 ? 0 : -EIO;
}

static int bounds(uint32_t sector, uint32_t count)
{
    if (!initialized) return -ENODEV;
    if (!count || sector >= sectors || count > sectors - sector) return -EINVAL;
    return 0;
}

int sdspi_read(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    trace_begin("read", sector, count);
    int result = bounds(sector, count);
    if (result < 0) return trace_finish(result);
    io_error = 0;
    int multiple = count > 1;
    result = command(multiple ? 18 : 17, block_addressed ? sector : sector * 512u);
    /* A lost R1 can leave CMD18 active. Stop it if its packet was attempted,
     * including a HAL failure during command/response; a rejected R1 cannot. */
    int needs_stop = multiple && (result == 0 || (result < 0 && trace.command_sent));
    if (result > 0) result = -EIO;
    while (result == 0 && count--) {
        trace.sector = sector;
        result = receive_data(buffer, 512);
        if (result < 0) break;
        buffer += 512;
        sector++;
        trace.done++;
    }
    if (needs_stop) return trace_finish(stop_read(result));
    deselect();
    return trace_finish(result < 0 ? result : io_error ? io_error : result);
}

int sdspi_write(const uint8_t *buffer, uint32_t sector, uint32_t count)
{
    trace_begin("write", sector, count);
    int result = bounds(sector, count);
    if (result < 0) return trace_finish(result);
    io_error = 0;
    while (count--) {
        trace.sector = sector;
        result = command(24, block_addressed ? sector : sector * 512u);
        if (result != 0) { result = result < 0 ? result : -EIO; break; }
        uint8_t prefix[2] = { 0xff, 0xfe };
        uint16_t crc = sdspi_crc16(buffer, 512);
        uint8_t trailer[2] = { (uint8_t)(crc >> 8), (uint8_t)crc };
        trace.crc_calculated = crc;
        trace.stage = "write-data";
        if (exchange(prefix, NULL, 2) < 0 || exchange(buffer, NULL, 512) < 0 ||
            exchange(trailer, NULL, 2) < 0) { result = io_error; break; }
        trace.stage = "write-response";
        uint8_t response = read_byte();
        trace.token = response;
        if ((response & 0x1f) != 0x05) { result = -EIO; break; }
        trace.stage = "write-ready";
        result = ready(1000);
        if (result < 0) break;
        buffer += 512;
        sector++;
        trace.done++;
    }
    deselect();
    return trace_finish(io_error ? io_error : result);
}

int sdspi_sync(void)
{
    trace_begin("sync", 0, 0);
    if (!initialized) return trace_finish(-ENODEV);
    io_error = 0;
    /* Busy completion alone does not report programming/ECC errors. CMD13's
     * R2 status must also be clear before a filesystem sync is acknowledged. */
    int result = command(13, 0);
    if (result == 0) {
        trace.stage = "status-r2";
        trace.r2 = read_byte();
        result = trace.r2 == 0 ? 0 : -EIO;
    }
    else if (result > 0) result = -EIO;
    deselect();
    return trace_finish(io_error ? io_error : result);
}
#endif /* SDSPI_HOST_TEST */
