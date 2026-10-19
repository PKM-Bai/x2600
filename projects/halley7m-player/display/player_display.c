/* X2600 NV12 composer backend. GPL-2.0-or-later. */
#include "player_display.h"
#include "panel_zc502.h"
#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600_hal.h>
#include <interrupt.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static LCD_HandleTypeDef lcd __attribute__((aligned(64)));
static struct lay_cfg layers[MAX_LAYER_DESC_NUM];
static player_video_layout_t layout;
static volatile player_display_frame_t pending_frame, scanout_frame;
static volatile player_display_completion_t completed;
static volatile player_display_stats_t counters;
static volatile bool pending_valid, completion_valid, scanout_valid;
static volatile bool pending_video_change, pending_video_enabled, pending_overlay_change;
static volatile bool overlay_valid, overlay_completion_valid;
static volatile bool overlay_queued;
static volatile uint32_t pending_descriptor, pending_y, pending_argb, pending_serial;
static volatile uint32_t scanout_argb, completed_serial;
static volatile uint32_t queued_argb, queued_serial;
static volatile uint64_t pending_started_us;
static volatile bool video_active;
static bool clocks_enabled, irq_registered, initialized, running, backlight_on;
static volatile bool backlight_starting;

static void display_irq(int irq, void *argument)
{
    (void)irq;
    (void)argument;
    uint32_t flags = READ_REG(lcd.Instance->DC_INT_FLAG) &
                     READ_REG(lcd.Instance->DC_INTC);
    if (flags & DC_INTC_INT_UOT_0) ++counters.underruns;
    if (flags & DC_INTC_INT_EOD_0) {
        uint64_t now = platform_time_us();
        if (counters.vblanks) {
            uint64_t elapsed = now - counters.last_eod_us;
            uint32_t period = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
            if (!counters.eod_period_min_us || period < counters.eod_period_min_us)
                counters.eod_period_min_us = period;
            if (period > counters.eod_period_max_us) counters.eod_period_max_us = period;
        }
        counters.last_eod_us = now;
        ++counters.vblanks;
        if (pending_valid) {
            /* SDK's debug ports expose the loaded descriptor, one word per
             * read. Reset the debug word counters, not the DMA engine. The
             * frame self-chain and layer source must both match this submit. */
            WRITE_REG(lcd.Instance->DC_CTRL, DC_CTRL_DES_CNT_RST_0);
            uint32_t descriptor = READ_REG(lcd.Instance->FRM_DES_READ);
            uint32_t physical_y = 0, site = 0;
            bool matches = descriptor == pending_descriptor;
            if (pending_video_enabled) {
                WRITE_REG(lcd.Instance->DC_CTRL, DC_CTRL_DES_CNT_RST_0);
                (void)READ_REG(lcd.Instance->LAY0_DES_READ);
                (void)READ_REG(lcd.Instance->LAY0_DES_READ);
                physical_y = READ_REG(lcd.Instance->LAY0_DES_READ);
                site = READ_REG(lcd.Instance->LAY0_SITE);
                matches = matches && physical_y == pending_y && site >= pending_y &&
                          site - pending_y < layout.total_bytes;
            }
            if (pending_argb) {
                WRITE_REG(lcd.Instance->DC_CTRL, DC_CTRL_DES_CNT_RST_0);
                (void)READ_REG(lcd.Instance->LAY1_DES_READ);
                (void)READ_REG(lcd.Instance->LAY1_DES_READ);
                uint32_t argb = READ_REG(lcd.Instance->LAY1_DES_READ);
                uint32_t ui_site = READ_REG(lcd.Instance->LAY1_SITE);
                matches = matches && argb == pending_argb && ui_site >= argb &&
                          ui_site - argb < PLAYER_DISPLAY_OVERLAY_BYTES;
            }
            counters.last_descriptor = descriptor;
            counters.last_physical_y = physical_y;
            counters.last_read_site = site;
            if (matches) {
                if (pending_video_change) {
                    completed.has_latched = pending_video_enabled;
                    completed.latched = pending_frame.token;
                    completed.has_retired = scanout_valid;
                    if (scanout_valid) completed.retired = scanout_frame.token;
                    completed.irq_sequence = counters.vblanks;
                    completed.irq_time_us = now;
                    completed.descriptor = descriptor;
                    completed.physical_y = physical_y;
                    completed.read_site = site;
                    if (pending_video_enabled) scanout_frame = pending_frame;
                    scanout_valid = pending_video_enabled;
                    completion_valid = true;
                    if (pending_video_enabled) {
                        uint64_t elapsed = now - pending_started_us;
                        uint32_t latency = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
                        if (latency > counters.video_latency_max_us) counters.video_latency_max_us = latency;
                        ++counters.confirmed;
                    }
                }
                if (pending_overlay_change) {
                    scanout_argb = pending_argb;
                    overlay_valid = true;
                    completed_serial = pending_serial;
                    overlay_completion_valid = true;
                    ++counters.overlay_confirmed;
                }
                pending_valid = false;
            } else ++counters.mismatches;
        }
    }
    WRITE_REG(lcd.Instance->DC_CLR_ST, flags);
}

static int stop_dma(void)
{
    HAL_SRDMA_Stop(&lcd, QCK_STOP);
    HAL_COMP_Stop(&lcd, QCK_STOP);
    uint64_t deadline = platform_time_us() + 100000u;
    while (__HAL_SRD_WORKING(&lcd) || __HAL_WORKING(&lcd)) {
        if (platform_time_us() >= deadline) return -ETIMEDOUT;
        vTaskDelay(1);
    }
    lcd.CompData.comp_stopped = 1;
    lcd.RdmaData.srd_stopped = 1;
    return 0;
}

static void reset_dsi(void)
{
    WRITE_REG(MIPI_DSI_Instance->LPCLK_CTRL, 0);
    WRITE_REG(MIPI_DSI_Instance->PWR_UP, 0);
    WRITE_REG(MIPI_DSI_Instance->PHY_RSTZ, 0);
}

int player_display_stop(void)
{
    running = false;
    video_active = false;
    initialized = false;
    if (!clocks_enabled) return 0;
    panel_zc502_power_off();
    backlight_on = false;
    backlight_starting = false;
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    int result = stop_dma();
    if (result) return result; /* Hardware may still read the caller's buffers. */
    if (irq_registered) {
        ll_release_irq(IRQ_INTC0_LCD);
        irq_registered = false;
    }
    WRITE_REG(lcd.Instance->DC_CLR_ST, 0xffffffffu);
    reset_dsi();
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_MIPI_DSI);
    CPM_GATE_Disable(CPM_Instance, CPM_CLKID_LCD);
    clocks_enabled = initialized = false;
    pending_valid = completion_valid = scanout_valid = false;
    video_active = false;
    overlay_valid = overlay_completion_valid = overlay_queued = false;
    return 0;
}

int player_display_init(void)
{
    if (initialized) return 0;
    if (clocks_enabled) return -EBUSY;
    int result = player_video_layout_nv12(720, 1280, &layout);
    if (result) return result;
    memset(&lcd, 0, sizeof(lcd));
    memset(layers, 0, sizeof(layers));
    memset((void *)&counters, 0, sizeof(counters));
    pending_valid = completion_valid = scanout_valid = false;
    video_active = false;
    overlay_valid = overlay_completion_valid = overlay_queued = false;
    scanout_argb = 0;
    lcd.Instance = DPU_Instance;
    lcd.pdata = &panel_zc502_data;
    LL_CPM_CGU_ConfigTypeDef clock = {
        .PLLMux = LPCDR_MPLL, .Div = 47, .Config = 0
    };
    if (CPM_CGU_LCD_Start(CPM_Instance, &clock) != HAL_OK) return -EIO;
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_LCD);
    CPM_GATE_Enable(CPM_Instance, CPM_CLKID_MIPI_DSI);
    clocks_enabled = true;
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    result = stop_dma();
    if (result) goto failed;
    reset_dsi();
    if (HAL_LCDC_Init(&lcd) != HAL_OK || HAL_LCDC_Enable(&lcd) != HAL_OK ||
        panel_zc502_result() != 0) {
        result = -EIO;
        goto failed;
    }
    ll_request_irq(IRQ_INTC0_LCD, display_irq, NULL);
    irq_registered = true;
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    WRITE_REG(lcd.Instance->DC_CLR_ST, 0xffffffffu);
    WRITE_REG(lcd.Instance->DC_INTC,
              DC_DPU_INTC_EOD_MSK_0 | DC_DPU_INTC_UOT_MSK_0);
    /* Unique orders are required even for disabled layers. GUI is topmost. */
    layers[0].lay_z_order = LAYER_Z_ORDER_2;
    layers[1].lay_z_order = LAYER_Z_ORDER_3;
    layers[2].lay_z_order = LAYER_Z_ORDER_1;
    layers[3].lay_z_order = LAYER_Z_ORDER_0;
    layers[0].format = LAYER_CFG_FORMAT_NV12;
    layers[0].color = COLOR_RGB;
    layers[0].g_alpha_en = ENABLE;
    layers[0].g_alpha_val = 255;
    layers[0].source_w = layout.width;
    layers[0].source_h = layout.height;
    layers[0].stride = layers[0].uv_stride = layout.stride;
    layers[0].scale_w = layout.width;
    layers[0].scale_h = layout.height;
    layers[0].domain_multi = 0;
    layers[1].format = LAYER_CFG_FORMAT_ARGB8888;
    layers[1].color = COLOR_RGB;
    layers[1].g_alpha_en = DISABLE; /* Use each pixel's alpha. */
    layers[1].g_alpha_val = 255;
    layers[1].source_w = layers[1].scale_w = 720;
    layers[1].source_h = layers[1].scale_h = 1280;
    layers[1].stride = layers[1].uv_stride = 720;
    layers[1].domain_multi = 0; /* LVGL outputs straight alpha. */
    initialized = true;
    return 0;
failed:
    {
        int stopped = player_display_stop();
        return stopped ? stopped : result;
    }
}

int player_display_start(void)
{
    if (!initialized) return -ENODEV;
    running = true;
    return 0;
}

void player_display_set_video_active(bool active)
{
    taskENTER_CRITICAL();
    video_active = active;
    taskEXIT_CRITICAL();
}

/* Caller holds the short submission critical section. Current video is reused
 * for GUI-only changes; it stays under the same scanout token throughout. */
static int submit(bool video_change, const player_display_frame_t *frame,
                  bool ui_change, uint32_t argb, uint32_t serial)
{
    pending_video_change = video_change;
    pending_video_enabled = video_change ? frame != NULL : scanout_valid;
    pending_overlay_change = ui_change;
    if (frame) pending_frame = *frame;
    else if (scanout_valid) pending_frame = scanout_frame;
    pending_y = pending_video_enabled ? CPHYSADDR(pending_frame.nv12) : 0;
    pending_argb = ui_change ? argb : (overlay_valid ? scanout_argb : 0);
    pending_serial = serial;
    layers[0].lay_en = pending_video_enabled;
    layers[0].addr = pending_y;
    layers[0].uv_addr = pending_y + layout.uv_offset;
    layers[1].lay_en = pending_argb != 0;
    layers[1].addr = pending_argb;
    if (HAL_COMP_Init(&lcd, layers) != HAL_OK) return -EIO;
    pending_descriptor = CPHYSADDR(&lcd.CompData.FrameDesc[lcd.CompData.comp_next_frame]);
    WRITE_REG(lcd.Instance->DC_CLR_ST, DC_INTC_INT_EOD_0);
    pending_started_us = platform_time_us();
    pending_valid = true;
    if (HAL_COMP_Start(&lcd) == HAL_OK) {
        if (video_change && frame) ++counters.video_submitted;
        if (ui_change) {
            if (video_change && frame) ++counters.overlay_merged;
            else if (!video_change) ++counters.overlay_only;
        }
        return 0;
    }
    pending_valid = false;
    return -EIO;
}

static int submit_with_queued_overlay(bool video_change, const player_display_frame_t *frame)
{
    bool take_overlay = overlay_queued;
    int result = submit(video_change, frame, take_overlay, queued_argb, queued_serial);
    if (!result && take_overlay) overlay_queued = false;
    return result;
}

int player_display_submit_frame(const player_display_frame_t *frame)
{
    if (!running) return -ENODEV;
    if (!frame || !player_video_buffer_valid(&layout, (uintptr_t)frame->nv12,
                                             frame->capacity)) return -EINVAL;
    uint32_t physical_y = CPHYSADDR(frame->nv12);
    /* This board has 64MiB DDR; aliases must resolve to a whole in-range frame. */
    if (physical_y > 0x04000000u - layout.total_bytes) return -EINVAL;
    int result = 0;
    taskENTER_CRITICAL();
    if (pending_valid || completion_valid) {
        ++counters.video_busy;
        result = -EAGAIN;
    }
    else if (scanout_valid && physical_y == CPHYSADDR(scanout_frame.nv12))
        result = -EINVAL;
    else {
        result = submit_with_queued_overlay(true, frame);
    }
    taskEXIT_CRITICAL();
    return result;
}

int player_display_clear_video(void)
{
    if (!running) return -ENODEV;
    int result;
    taskENTER_CRITICAL();
    if (pending_valid || completion_valid) result = -EAGAIN;
    else result = scanout_valid ? submit_with_queued_overlay(true, NULL) : 0;
    taskEXIT_CRITICAL();
    return result;
}

int player_display_submit_overlay(const uint32_t *argb, uint32_t capacity, uint32_t serial)
{
    if (!running) return -ENODEV;
    if (!argb || ((uintptr_t)argb & 63u) || capacity < PLAYER_DISPLAY_OVERLAY_BYTES || !serial)
        return -EINVAL;
    uint32_t physical = CPHYSADDR(argb);
    if (!physical || physical > 0x04000000u - PLAYER_DISPLAY_OVERLAY_BYTES) return -EINVAL;
    int result;
    taskENTER_CRITICAL();
    if (overlay_completion_valid || overlay_queued || (pending_valid && pending_overlay_change)) result = -EAGAIN;
    else if (overlay_valid && physical == scanout_argb) result = -EINVAL;
    else if (pending_valid || video_active) {
        /* Pin one GUI request behind the in-flight video transaction. The
         * worker's next video submit merges it, so GUI cannot starve behind
         * a stream of higher-priority video submissions. */
        queued_argb = physical;
        queued_serial = serial;
        overlay_queued = true;
        ++counters.overlay_queued_requests;
        result = 0;
    } else result = submit(false, NULL, true, physical, serial);
    if (!result) ++counters.overlay_submitted;
    taskEXIT_CRITICAL();
    return result;
}

static int enable_backlight(void)
{
    /* GUI and video completions can be polled by different tasks. Claim the
     * slow PWM settling once; never sleep with interrupts masked. */
    taskENTER_CRITICAL();
    bool ready = backlight_on;
    bool busy = backlight_starting;
    if (!ready && !busy) backlight_starting = true;
    taskEXIT_CRITICAL();
    if (ready) return 0;
    if (busy) return 1;
    panel_zc502_backlight_on();
    int result = panel_zc502_result() ? -EIO : 0;
    taskENTER_CRITICAL();
    backlight_starting = false;
    backlight_on = !result;
    taskEXIT_CRITICAL();
    return result;
}

int player_display_poll_overlay_completion(uint32_t *serial)
{
    if (!serial) return -EINVAL;
    int queued_result = 0;
    taskENTER_CRITICAL();
    /* With paused/no video, GUI polling itself drives the queued update. */
    if (overlay_queued && !pending_valid && !video_active)
        queued_result = submit_with_queued_overlay(false, NULL);
    taskEXIT_CRITICAL();
    if (queued_result) return queued_result;
    if (!overlay_completion_valid) return 0;
    int result = enable_backlight();
    if (result) return result < 0 ? result : 0;
    taskENTER_CRITICAL();
    *serial = completed_serial;
    overlay_completion_valid = false;
    taskEXIT_CRITICAL();
    return 1;
}

int player_display_poll_completion(player_display_completion_t *completion)
{
    if (!completion) return -EINVAL;
    int result = 0;
    taskENTER_CRITICAL();
    if (completion_valid) {
        *completion = completed;
        result = 1;
    }
    taskEXIT_CRITICAL();
    if (result && !backlight_on) {
        int error = enable_backlight();
        if (error) return error < 0 ? error : 0;
    }
    if (result) {
        taskENTER_CRITICAL();
        completion_valid = false;
        taskEXIT_CRITICAL();
    }
    return result;
}

void player_display_get_stats(player_display_stats_t *stats)
{
    taskENTER_CRITICAL();
    *stats = counters;
    stats->pending = pending_valid || overlay_queued;
    stats->scanout = scanout_valid;
    stats->overlay_scanout = overlay_valid;
    stats->video_active = video_active;
    taskEXIT_CRITICAL();
}

int player_display_test_pattern(bool enabled)
{
    if (!initialized) return -ENODEV;
    if (enabled) SET_BIT(MIPI_DSI_Instance->VID_MODE_CFG, VID_MODE_CFG_VPG_EN);
    else CLEAR_BIT(MIPI_DSI_Instance->VID_MODE_CFG, VID_MODE_CFG_VPG_EN);
    return 0;
}

void player_display_print_status(void)
{
    player_display_stats_t timing;
    player_display_get_stats(&timing);
    printf("DISPLAY_SCHED active=%u video_submitted=%lu video_busy=%lu overlay_submitted=%lu overlay_queued=%lu overlay_merged=%lu overlay_only=%lu video_confirmed=%lu overlay_confirmed=%lu eod_period_min_us=%lu eod_period_max_us=%lu video_latency_max_us=%lu\n",
           (unsigned)timing.video_active, (unsigned long)timing.video_submitted, (unsigned long)timing.video_busy,
           (unsigned long)timing.overlay_submitted, (unsigned long)timing.overlay_queued_requests,
           (unsigned long)timing.overlay_merged, (unsigned long)timing.overlay_only,
           (unsigned long)timing.confirmed, (unsigned long)timing.overlay_confirmed,
           (unsigned long)timing.eod_period_min_us, (unsigned long)timing.eod_period_max_us,
           (unsigned long)timing.video_latency_max_us);
    printf("DISPLAY clocks=%d init=%d running=%d backlight=%d panel_result=%d pending=%d scanout=%d\n",
           clocks_enabled, initialized, running, backlight_on, panel_zc502_result(),
           pending_valid, scanout_valid);
    printf("GPIOC pin=%08lx int=%08lx mask=%08lx pat1=%08lx pat0=%08lx\n",
           PC_Instance->PxPINL, PC_Instance->PxINT, PC_Instance->PxMSK,
           PC_Instance->PxPAT1, PC_Instance->PxPAT0);
    printf("BACKLIGHT pwm_en=%08lx mode=%08lx initial=%08lx idle=%08lx cc15=%08lx wc15=%08lx\n",
           PWM_Instance->PWMEN, PWM_Instance->PWMMS, PWM_Instance->PWMINL,
           PWM_Instance->PWMIDL, PWM_Instance->PWMCCn[15], PWM_Instance->PWMWCn[15]);
    printf("DISPLAY_CPM clkgr0=%08lx clkgr1=%08lx pwmcdr=%08lx lpcdr=%08lx\n",
           CPM_Instance->CLKGR0, CPM_Instance->CLKGR1, CPM_Instance->PWMCDR, CPM_Instance->LPCDR);
    if (!clocks_enabled) return;
    const uint16_t dpu_offsets[] = {
        0x0000, 0x1000, 0x2004, 0x200c, 0x2010, 0x2014, 0x2018,
        0x2204, 0x3110, 0x8000, 0x9000, 0x9004, 0x9008, 0x900c, 0x9010, 0x9014
    };
    const uint16_t dsi_offsets[] = {
        0x0004, 0x0008, 0x0010, 0x0014, 0x002c, 0x0034, 0x0038, 0x003c,
        0x0048, 0x004c, 0x0050, 0x0054, 0x0058, 0x005c, 0x0060, 0x0068,
        0x0074, 0x0094, 0x0098, 0x009c, 0x00a0, 0x00a4, 0x00b0, 0x00bc, 0x00c0
    };
    for (unsigned i = 0; i < sizeof(dpu_offsets) / sizeof(*dpu_offsets); ++i)
        printf("DPU %04x=%08lx\n", dpu_offsets[i],
               *(volatile unsigned long *)((uintptr_t)DPU_Instance + dpu_offsets[i]));
    for (unsigned i = 0; i < sizeof(dsi_offsets) / sizeof(*dsi_offsets); ++i)
        printf("DSI %04x=%08lx\n", dsi_offsets[i],
               *(volatile unsigned long *)((uintptr_t)MIPI_DSI_Instance + dsi_offsets[i]));
    unsigned frame = lcd.CompData.comp_next_frame;
    const volatile uint32_t *desc = (const volatile uint32_t *)(uintptr_t)
        (CPHYSADDR(&lcd.CompData.FrameDesc[frame]) | 0xa0000000u);
    const volatile uint32_t *layer = (const volatile uint32_t *)(uintptr_t)
        (CPHYSADDR(&lcd.CompData.LayerDesc[frame][0]) | 0xa0000000u);
    for (unsigned i = 0; i < 16; ++i)
        printf("DISPLAY_DESC %u frame=%08lx layer0=%08lx\n", i,
               (unsigned long)desc[i], (unsigned long)layer[i]);
}
