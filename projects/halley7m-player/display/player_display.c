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
static volatile uint32_t pending_descriptor, pending_y;
static bool clocks_enabled, irq_registered, initialized, running, backlight_on;

static void display_irq(int irq, void *argument)
{
    (void)irq;
    (void)argument;
    uint32_t flags = READ_REG(lcd.Instance->DC_INT_FLAG) &
                     READ_REG(lcd.Instance->DC_INTC);
    if (flags & DC_INTC_INT_UOT_0) ++counters.underruns;
    if (flags & DC_INTC_INT_EOD_0) {
        ++counters.vblanks;
        if (pending_valid) {
            /* SDK's debug ports expose the loaded descriptor, one word per
             * read. Reset the debug word counters, not the DMA engine. The
             * frame self-chain and layer source must both match this submit. */
            WRITE_REG(lcd.Instance->DC_CTRL, DC_CTRL_DES_CNT_RST_0);
            uint32_t descriptor = READ_REG(lcd.Instance->FRM_DES_READ);
            WRITE_REG(lcd.Instance->DC_CTRL, DC_CTRL_DES_CNT_RST_0);
            (void)READ_REG(lcd.Instance->LAY0_DES_READ); /* LayerSize */
            (void)READ_REG(lcd.Instance->LAY0_DES_READ); /* LayerCfg */
            uint32_t physical_y = READ_REG(lcd.Instance->LAY0_DES_READ);
            uint32_t site = READ_REG(lcd.Instance->LAY0_SITE);
            counters.last_descriptor = descriptor;
            counters.last_physical_y = physical_y;
            counters.last_read_site = site;
            if (descriptor == pending_descriptor && physical_y == pending_y &&
                site >= pending_y && site - pending_y < layout.total_bytes) {
                completed.latched = pending_frame.token;
                completed.has_retired = scanout_valid;
                if (scanout_valid) completed.retired = scanout_frame.token;
                completed.irq_sequence = counters.vblanks;
                completed.irq_time_us = platform_time_us();
                completed.descriptor = descriptor;
                completed.physical_y = physical_y;
                completed.read_site = site;
                scanout_frame = pending_frame;
                scanout_valid = true;
                pending_valid = false;
                completion_valid = true;
                ++counters.confirmed;
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
    initialized = false;
    if (!clocks_enabled) return 0;
    panel_zc502_power_off();
    backlight_on = false;
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
    layers[0].lay_en = ENABLE;
    /* The mixer needs a unique order even for disabled layers. Match the
     * SDK composer example: the only active video layer is the top layer. */
    for (unsigned i = 0; i < MAX_LAYER_DESC_NUM; ++i)
        layers[i].lay_z_order = LAYER_Z_ORDER_3 - i;
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
    /* Layer 1 remains disabled; a future ARGB UI can use straight alpha. */
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
    if (pending_valid || completion_valid) result = -EAGAIN;
    else if (scanout_valid && physical_y == CPHYSADDR(scanout_frame.nv12))
        result = -EINVAL;
    else {
        layers[0].addr = physical_y;
        layers[0].uv_addr = physical_y + layout.uv_offset;
        if (HAL_COMP_Init(&lcd, layers) != HAL_OK) result = -EIO;
        else {
            pending_frame = *frame;
            pending_y = physical_y;
            pending_descriptor = CPHYSADDR(&lcd.CompData.FrameDesc[lcd.CompData.comp_next_frame]);
            WRITE_REG(lcd.Instance->DC_CLR_ST, DC_INTC_INT_EOD_0);
            pending_valid = true;
            if (HAL_COMP_Start(&lcd) != HAL_OK) {
                pending_valid = false;
                result = -EIO;
            }
        }
    }
    taskEXIT_CRITICAL();
    return result;
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
        panel_zc502_backlight_on();
        if (panel_zc502_result() != 0) return -EIO;
        backlight_on = true;
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
    stats->pending = pending_valid;
    stats->scanout = scanout_valid;
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
