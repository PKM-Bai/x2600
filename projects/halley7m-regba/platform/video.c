/* X2600 FreeRTOS ReGBA display backend. GPL-2.0-or-later. */
#include "video.h"
#include "video_blit.h"
#include "ui_draw.h"
#include "panel_zc502.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <cache.h>
#include <interrupt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 720, HEIGHT = 1280, PIXELS = WIDTH * HEIGHT,
       FRAME_BYTES = PIXELS * sizeof(uint32_t) };

static LCD_HandleTypeDef lcd __attribute__((aligned(64)));
static struct srdma_cfg rdma;
static void *allocation;
static uint32_t *buffers[2];
static volatile int active;
static int clocks_enabled, irq_registered;
static unsigned front;
static unsigned test_pattern_mask;
static unsigned stats_drawn_mask;
static game_stats_t drawn_stats[2];
static int pending = -1;
static uint32_t submitted_vblank, last_present_vblank;
static volatile uint32_t vblanks, underruns, presented, skipped;
static volatile uint32_t blit_last_us, blit_max_us;

static void display_irq(int irq, void *argument)
{
    (void)irq;
    (void)argument;
    uint32_t flags = READ_REG(lcd.Instance->DC_INT_FLAG) &
                     READ_REG(lcd.Instance->DC_INTC);
    if (flags & DC_INTC_INT_EOD_0)
        ++vblanks;
    if (flags & DC_INTC_INT_UOT_0)
        ++underruns;
    /* Clear all enabled causes in one write. No HAL lock, logging or RTOS
     * calls are permitted in this interrupt. Flag and clear bit positions
     * match for EOD/UOT. */
    WRITE_REG(lcd.Instance->DC_CLR_ST, flags);
}

static int stop_dma(void)
{
    HAL_SRDMA_Stop(&lcd, QCK_STOP);
    HAL_COMP_Stop(&lcd, QCK_STOP);
    uint64_t deadline = platform_time_us() + 100000u;
    while (__HAL_SRD_WORKING(&lcd) || __HAL_WORKING(&lcd)) {
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
        vTaskDelay(1);
    }
    lcd.RdmaData.srd_stopped = 1;
    return 0;
}

static void reset_dsi(void)
{
    /* Video mode leaves the HS clock request asserted. Clear it before
     * resetting the host/PHY so the next LP command initialization can
     * observe clock-lane stop state, including after an off/on cycle. */
    WRITE_REG(MIPI_DSI_Instance->LPCLK_CTRL, 0);
    WRITE_REG(MIPI_DSI_Instance->PWR_UP, 0);
    WRITE_REG(MIPI_DSI_Instance->PHY_RSTZ, 0);
}

void video_off(void)
{
    active = 0;
    if (!clocks_enabled) {
        free(allocation);
        allocation = NULL;
        buffers[0] = buffers[1] = NULL;
        return;
    }
    panel_zc502_power_off();
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    if (irq_registered) {
        ll_release_irq(IRQ_INTC0_LCD);
        irq_registered = 0;
    }
    if (stop_dma() != 0) {
        /* A stopped controller must be confirmed before returning memory
         * to the ROM heap. Leave it reserved if hardware did not stop. */
        puts("lcd off: DMA stop timeout, framebuffer retained");
        return;
    }
    WRITE_REG(lcd.Instance->DC_CLR_ST, 0xFFFFFFFFu);
    reset_dsi();
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_MIPI_DSI);
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_LCD);
    /* This SDK declares CPM_CGU_LCD_Stop but compiles it out. Once DMA
     * is idle, closing the two controller gates is sufficient here. */
    clocks_enabled = 0;
    pending = -1;
    free(allocation);
    allocation = NULL;
    buffers[0] = buffers[1] = NULL;
}

int video_init(void)
{
    LL_CPM_CGU_ConfigTypeDef clock = {
        .PLLMux = LPCDR_MPLL, .Div = 47, .Config = 0
    };
    int result = -EIO;
    if (active)
        return 0;
    if (allocation || clocks_enabled)
        return -EBUSY;

    allocation = malloc(2u * FRAME_BYTES + 63u);
    if (!allocation)
        return -ENOMEM;
    buffers[0] = (uint32_t *)(((uintptr_t)allocation + 63u) & ~(uintptr_t)63u);
    buffers[1] = buffers[0] + PIXELS;
    stats_drawn_mask = 0;
    memset(buffers[0], 0, 2u * FRAME_BYTES);
    CleanDCache_by_Addr((unsigned long *)buffers[0], 2u * FRAME_BYTES);
    memset(&lcd, 0, sizeof(lcd));
    lcd.Instance = DPU_Instance;
    lcd.pdata = &panel_zc502_data;
    front = 0;
    test_pattern_mask = 0;
    pending = -1;
    vblanks = underruns = presented = skipped = 0;
    blit_last_us = blit_max_us = 0;

    if (CPM_CGU_LCD_Start(CPM_Instance, &clock) != HAL_OK)
        goto failed;
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_LCD);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_MIPI_DSI);
    clocks_enabled = 1;
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    if (stop_dma() != 0) {
        result = -ETIMEDOUT;
        goto failed;
    }
    reset_dsi();
    if (HAL_LCDC_Init(&lcd) != HAL_OK)
        goto failed;
    ll_request_irq(IRQ_INTC0_LCD, display_irq, NULL);
    irq_registered = 1;
    if (HAL_LCDC_Enable(&lcd) != HAL_OK || panel_zc502_result() != 0)
        goto failed;
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    WRITE_REG(lcd.Instance->DC_CLR_ST, 0xFFFFFFFFu);
    WRITE_REG(lcd.Instance->DC_INTC,
              DC_DPU_INTC_EOD_MSK_0 | DC_DPU_INTC_UOT_MSK_0);
    rdma.src_format = FMT_RGB888;
    rdma.src_color = COLOR_RGB;
    rdma.buffer = buffers[0];
    if (HAL_SRDMA_Init(&lcd, &rdma) != HAL_OK ||
        HAL_SRDMA_Start(&lcd) != HAL_OK)
        goto failed;

    /* Initialization may yield; the emulation frame path below never waits.
     * Keep the backlight dark until the controller has produced a frame. */
    uint64_t deadline = platform_time_us() + 100000u;
    while (!vblanks) {
        if (platform_time_us() >= deadline) {
            result = -ETIMEDOUT;
            goto failed;
        }
        vTaskDelay(1);
    }
    panel_zc502_backlight_on();
    if (panel_zc502_result() != 0)
        goto failed;
    last_present_vblank = vblanks - 1u;
    active = 1;
    printf("lcd on: zc50289hshd02 720x1280 portrait 30Hz, stride=2880 double_buffer=%u\n",
           2u * (unsigned)FRAME_BYTES);
    return 0;

failed:
    video_off();
    return result;
}

static int backbuffer_available(void)
{
    uint32_t now = vblanks;
    if (pending >= 0) {
        uintptr_t start = VIRT_TO_PHYS(buffers[pending]);
        uintptr_t site = READ_REG(lcd.Instance->DC_RDMA_SITE);
        /* Pan_Display queues a new chain; it does not prove the controller
         * has stopped reading the previous frame. Reclaim the old front
         * only after another display end and a read site in the new frame.
         * These ownership checks still require on-board tearing validation. */
        if (now == submitted_vblank || site < start || site >= start + FRAME_BYTES)
            return 0;
        front = (unsigned)pending;
        pending = -1;
    }
    if (now == last_present_vblank)
        return 0;
    return 1;
}

static int queue_frame(unsigned back)
{
    rdma.buffer = buffers[back];
    submitted_vblank = vblanks;
    if (HAL_SRDMA_Pan_Display(&lcd, &rdma) != HAL_OK)
        return -EIO;
    pending = (int)back;
    last_present_vblank = submitted_vblank;
    ++presented;
    return 0;
}

void video_present_gba(const uint16_t *rgb555, unsigned source_stride_pixels)
{
    if (!active)
        return;
    if (!backbuffer_available()) {
        ++skipped;
        return;
    }
    unsigned back = front ^ 1u;
    uint64_t started = platform_time_us();
    int clear_pattern = test_pattern_mask & (1u << back);
    if (clear_pattern) {
        memset(buffers[back], 0, FRAME_BYTES);
        test_pattern_mask &= ~(1u << back);
    }
    video_blit_gba(buffers[back], WIDTH, rgb555, source_stride_pixels);
    game_stats_t stats;
    game_stats_get(&stats);
    int draw_stats = clear_pattern || !(stats_drawn_mask & (1u << back)) ||
        drawn_stats[back].valid != stats.valid ||
        drawn_stats[back].cpu_tenths != stats.cpu_tenths ||
        drawn_stats[back].fps_tenths != stats.fps_tenths;
    if (draw_stats) {
        ui_draw_game_stats(buffers[back], WIDTH, HEIGHT, &stats);
        drawn_stats[back] = stats;
        stats_drawn_mask |= 1u << back;
    }
    /* Publish changed game/HUD rows through L2 to DDR
     * before making this framebuffer visible to the display DMA. */
    if (clear_pattern)
        CleanDCache_by_Addr((unsigned long *)buffers[back], FRAME_BYTES);
    else {
        if (draw_stats)
            CleanDCache_by_Addr((unsigned long *)(buffers[back] + UI_GAME_STATS_Y * WIDTH),
                               UI_GAME_STATS_HEIGHT * WIDTH * sizeof(uint32_t));
        CleanDCache_by_Addr((unsigned long *)(buffers[back] + 400u * WIDTH),
                           480u * WIDTH * sizeof(uint32_t));
    }
    blit_last_us = (uint32_t)(platform_time_us() - started);
    if (blit_last_us > blit_max_us)
        blit_max_us = blit_last_us;
    if (queue_frame(back) != 0)
        ++skipped;
}

int video_present_ui(video_draw_fn draw, void *context)
{
    if (!draw)
        return -EINVAL;
    if (!active)
        return -ENODEV;
    if (!backbuffer_available()) {
        ++skipped;
        return -EAGAIN;
    }
    unsigned back = front ^ 1u;
    uint64_t started = platform_time_us();
    CLEAR_BIT(MIPI_DSI_Instance->VID_MODE_CFG, VID_MODE_CFG_VPG_EN);
    draw(buffers[back], WIDTH, HEIGHT, context);
    /* UI owns the whole image, including the usual black game borders.
     * Clear each affected buffer once before returning to game rendering. */
    test_pattern_mask |= 1u << back;
    CleanDCache_by_Addr((unsigned long *)buffers[back], FRAME_BYTES);
    blit_last_us = (uint32_t)(platform_time_us() - started);
    if (blit_last_us > blit_max_us)
        blit_max_us = blit_last_us;
    int result = queue_frame(back);
    if (result != 0)
        ++skipped;
    return result;
}

int video_test_pattern(void)
{
    int result = video_init();
    if (result)
        return result;
    CLEAR_BIT(MIPI_DSI_Instance->VID_MODE_CFG, VID_MODE_CFG_VPG_EN);
    /* A single static portrait chart: RGB bars in the centered game area,
     * white outer border, blue top marker and red bottom marker. */
    uint64_t deadline = platform_time_us() + 100000u;
    while (!backbuffer_available()) {
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
        vTaskDelay(1);
    }
    unsigned back = front ^ 1u;
    static const uint32_t colors[] = {
        0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF00FF00u,
        0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u
    };
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint32_t color = 0xFF000000u;
            if (y >= 400 && y < 880)
                color = colors[x / 90u];
            if (y >= 40 && y < 56 && x >= 40 && x < 200)
                color = 0xFF0000FFu;
            if (y >= 1224 && y < 1240 && x >= 520 && x < 680)
                color = 0xFFFF0000u;
            if (x < 4 || x >= WIDTH - 4 || y < 4 || y >= HEIGHT - 4 ||
                y == 399 || y == 880)
                color = 0xFFFFFFFFu;
            buffers[back][y * WIDTH + x] = color;
        }
    }
    test_pattern_mask |= 1u << back;
    CleanDCache_by_Addr((unsigned long *)buffers[back], FRAME_BYTES);
    return queue_frame(back);
}

int video_dsi_pattern(void)
{
    int result = video_init();
    if (!result)
        SET_BIT(MIPI_DSI_Instance->VID_MODE_CFG, VID_MODE_CFG_VPG_EN);
    return result;
}

void video_status(void)
{
    printf("lcd active=%d vblank=%lu presented=%lu skipped=%lu underrun=%lu blit_us=%lu max_us=%lu allocated=%u\n",
           active, (unsigned long)vblanks, (unsigned long)presented,
           (unsigned long)skipped, (unsigned long)underruns,
           (unsigned long)blit_last_us, (unsigned long)blit_max_us,
           allocation ? 2u * (unsigned)FRAME_BYTES : 0u);
}

void video_diagnostics(void)
{
    if (!clocks_enabled) {
        puts("lcd diag: clocks off");
        return;
    }
    static const uint16_t dpu_offsets[] = {
        0x0000, 0x1000, 0x2004, 0x200c, 0x2010, 0x2014,
        0x2018, 0x2204, 0x3110, 0x8000,
        0x9000, 0x9004, 0x9008, 0x900c, 0x9010, 0x9014
    };
    static const uint16_t dsi_offsets[] = {
        0x0004, 0x0008, 0x0010, 0x0014, 0x002c, 0x0034,
        0x0038, 0x003c, 0x0048, 0x004c, 0x0050, 0x0054,
        0x0058, 0x005c, 0x0060, 0x0068, 0x0074, 0x0094,
        0x0098, 0x009c, 0x00a0, 0x00a4, 0x00b0, 0x00bc, 0x00c0
    };
    printf("lcd diag front=%u pending=%d buffers=%08lx/%08lx site=%08lx chain=%08lx\n",
           front, pending, (unsigned long)VIRT_TO_PHYS(buffers[0]),
           (unsigned long)VIRT_TO_PHYS(buffers[1]),
           (unsigned long)READ_REG(lcd.Instance->DC_RDMA_SITE),
           (unsigned long)READ_REG(lcd.Instance->DC_SRD_CHAIN_ADDR));
    for (unsigned d = 0; d < MAX_SRDMA_DESC_NUM; ++d) {
        volatile uint32_t *desc = (volatile uint32_t *)
            (VIRT_TO_PHYS(&lcd.RdmaData.RdmaDesc[d]) | 0xa0000000u);
        printf("lcd descriptor[%u]=%08lx %08lx %08lx %08lx %08lx\n", d,
               (unsigned long)desc[0], (unsigned long)desc[1],
               (unsigned long)desc[2], (unsigned long)desc[3],
               (unsigned long)desc[4]);
    }
    for (unsigned i = 0; i < sizeof(dpu_offsets) / sizeof(dpu_offsets[0]); ++i) {
        uintptr_t address = (uintptr_t)DPU_Instance + dpu_offsets[i];
        printf("DPU %04x=%08lx\n", dpu_offsets[i],
               (unsigned long)*(volatile uint32_t *)address);
    }
    for (unsigned i = 0; i < sizeof(dsi_offsets) / sizeof(dsi_offsets[0]); ++i) {
        uintptr_t address = (uintptr_t)MIPI_DSI_Instance + dsi_offsets[i];
        printf("DSI %04x=%08lx\n", dsi_offsets[i],
               (unsigned long)*(volatile uint32_t *)address);
    }
    for (unsigned b = 0; b < 2; ++b) {
        volatile uint32_t *uncached =
            (volatile uint32_t *)(VIRT_TO_PHYS(buffers[b]) | 0xa0000000u);
        printf("lcd DDR[%u] border=%08lx center=%08lx\n", b,
               (unsigned long)uncached[0],
               (unsigned long)uncached[600u * WIDTH + 100u]);
    }
}
