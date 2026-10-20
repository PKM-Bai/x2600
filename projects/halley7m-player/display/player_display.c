/* X2600 NV12 composer backend. GPL-2.0-or-later. */
#include "player_display.h"
#include "player_rotator.h"
#include "panel_zc502.h"
#include "platform_runtime.h"
#include "FreeRTOS.h"
#include "task.h"
#include <x2600_hal.h>
#include <interrupt.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cache.h>

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
static player_rotator_t rotator;
static player_rotator_token_t pending_rotator;
static void *writeback_allocations[PLAYER_ROTATOR_SLOTS];
static volatile bool rotated, rotator_enabled;
static bool rotator_irq_registered;
static volatile bool rotator_stopping, pending_written;
static bool queued_fetched, queued_start_seen, queued_old_read_ended;
static player_rotator_token_t queued_start_token;
static uint32_t pending_writeback_us;
static bool gui_attached;
static volatile unsigned route_phase, route_ack;
static volatile uint32_t route_generation, requested_width, requested_height;
static volatile int route_error;
static uint64_t route_deadline;
/* First events and the first failure survive subsequent IRQs. No UART in ISR. */
typedef struct {
    uint32_t status, site, time_us, eod_sequence, writeback_us, kind;
    int queued, reading, last, latest, result;
} rotator_event_t;
static rotator_event_t rotator_events[16], rotator_first_error;
static unsigned rotator_event_count;
static bool rotator_has_error;

static void record_rotator_event(rotator_event_t event)
{
    event.time_us=(uint32_t)platform_time_us();
    event.eod_sequence=counters.vblanks;
    event.writeback_us=pending_writeback_us;
    if (rotator_event_count<sizeof(rotator_events)/sizeof(*rotator_events))
        rotator_events[rotator_event_count++]=event;
    if (event.result && !rotator_has_error) { rotator_first_error=event; rotator_has_error=true; }
}

#ifndef PLAYER_ROTATOR_MMIO
#define PLAYER_ROTATOR_MMIO ((volatile unsigned long *)(uintptr_t)0xb3070000u)
#endif
#define ROT_REG(offset) PLAYER_ROTATOR_MMIO[(offset)/4u]

static void post_rotator(player_rotator_token_t token)
{
    queued_start_token=token;
    queued_fetched=queued_start_seen=queued_old_read_ended=false;
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_BUFFER),rotator.slots[token.slot].physical|1u);
}

static int resolve_rotator_start(void)
{
    if (!rotator_enabled || rotator_stopping || route_error || rotator.queued<0 ||
        !queued_fetched || !queued_start_seen) return 0;
    uint32_t site=READ_REG(ROT_REG(PLAYER_ROTATOR_READ_SITE));
    rotator_event_t event={0};
    event.status=READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS)); event.site=site; event.kind=1;
    event.queued=rotator.queued; event.reading=rotator.reading;
    event.last=rotator.last; event.latest=rotator.latest_started;
    int result=player_rotator_resolve_start(&rotator,queued_start_token,site,queued_old_read_ended);
    if (result) {
        event.result=result<0 ? result : 0;
        record_rotator_event(event);
        if (result<0) { route_error=result; ++counters.rotator_errors; }
        else { ++counters.rotator_late_binds; queued_start_seen=false; }
    }
    return result<0 ? result : 0;
}

static void rotator_queue_ready(void)
{
    if (!pending_valid || !pending_written || rotator_stopping) return;
    if (!(rotator.slots[pending_rotator.slot].flags & PLAYER_ROT_READY)) return;
    int result=player_rotator_queue(&rotator,pending_rotator);
    if (!result) post_rotator(pending_rotator);
    else if (result!=-EAGAIN) { route_error=result; ++counters.rotator_errors; }
}

static void rotator_service(void)
{
    if (!rotator_enabled) return;
    uint32_t status=READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS));
    uint32_t flags=status&0x1eu;
    if (!flags) return;
    rotator_event_t event={0};
    event.status=status; event.site=READ_REG(ROT_REG(PLAYER_ROTATOR_READ_SITE));
    event.queued=rotator.queued; event.reading=rotator.reading;
    event.last=rotator.last; event.latest=rotator.latest_started;
    /* Clear the captured flags before posting another command. */
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CLEAR),flags);
    (void)READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS)); /* Drain the peripheral clear write. */
    if (rotator_stopping) {
        /* No frame is being presented during quiescence. Keep every slot
         * pinned until ACK/idle; do not interpret a stopped RDMA debug site. */
        record_rotator_event(event);
        return;
    }
    if (rotator.queued>=0) {
        if (flags&PLAYER_ROTATOR_FETCH) queued_fetched=true;
        if ((flags&PLAYER_ROTATOR_SOF) && queued_fetched) queued_start_seen=true;
        if (flags&PLAYER_ROTATOR_EOF) queued_old_read_ended=true;
    }
    bool ended=(flags&PLAYER_ROTATOR_EOF)!=0;
    int result=0;
    if (ended && rotator.reading>=0) {
        result=player_rotator_eof(&rotator); ++counters.rotator_eof; ended=false;
    }
    if (!result && (flags&PLAYER_ROTATOR_SOF)) {
        int queued=rotator.queued;
        result=player_rotator_sof(&rotator,event.site);
        ++counters.rotator_sof;
        if (!result && rotator.queued==queued) ++counters.rotator_held_sof;
    }
    if (!result && ended) { result=player_rotator_eof(&rotator); ++counters.rotator_eof; }
    event.result=result;
    record_rotator_event(event);
    if (result) { route_error=result; ++counters.rotator_errors; return; }
    if (rotator_stopping || route_error) return;
    if (resolve_rotator_start()) return;
    rotator_queue_ready();
    if (rotator.reading<0 && rotator.queued<0) {
        player_rotator_token_t repeat;
        if (!player_rotator_repeat(&rotator,&repeat)) {
            post_rotator(repeat);
            ++counters.rotator_repeats;
        }
    }
}

static void rotator_irq(int irq, void *argument)
{ (void)irq; (void)argument; rotator_service(); }

static bool pending_matches(uint32_t *descriptor, uint32_t *physical_y, uint32_t *site)
{
    WRITE_REG(lcd.Instance->DC_CTRL,DC_CTRL_DES_CNT_RST_0);
    *descriptor=READ_REG(lcd.Instance->FRM_DES_READ);
    *physical_y=*site=0;
    bool matches=*descriptor==pending_descriptor;
    if (pending_video_enabled) {
        WRITE_REG(lcd.Instance->DC_CTRL,DC_CTRL_DES_CNT_RST_0);
        (void)READ_REG(lcd.Instance->LAY0_DES_READ);
        (void)READ_REG(lcd.Instance->LAY0_DES_READ);
        *physical_y=READ_REG(lcd.Instance->LAY0_DES_READ);
        *site=READ_REG(lcd.Instance->LAY0_SITE);
        matches=matches && *physical_y==pending_y && *site>=pending_y &&
                *site-pending_y<layout.total_bytes;
    }
    if (pending_argb) {
        WRITE_REG(lcd.Instance->DC_CTRL,DC_CTRL_DES_CNT_RST_0);
        (void)READ_REG(lcd.Instance->LAY1_DES_READ);
        (void)READ_REG(lcd.Instance->LAY1_DES_READ);
        uint32_t argb=READ_REG(lcd.Instance->LAY1_DES_READ);
        uint32_t ui_site=READ_REG(lcd.Instance->LAY1_SITE);
        matches=matches && argb==pending_argb && ui_site>=argb &&
                ui_site-argb<PLAYER_DISPLAY_OVERLAY_BYTES;
    }
    return matches;
}

static void display_irq(int irq, void *argument)
{
    (void)irq;
    (void)argument;
    uint32_t flags = READ_REG(lcd.Instance->DC_INT_FLAG) &
                     READ_REG(lcd.Instance->DC_INTC);
    if (rotated) rotator_service(); /* SOF may precede this EOD with its IRQ still pending. */
    if (rotated && (flags&DC_INTC_INT_EOD_0)) (void)resolve_rotator_start();
    if (flags & DC_INTC_INT_UOT_0) ++counters.underruns;
    if (flags & DC_INTC_INT_OOW_0) { ++counters.rotator_errors; route_error=-EIO; }
    if (rotated && (flags&DC_INTC_INT_EOW_0) && pending_valid && !pending_written) {
        uint32_t descriptor, y, site;
        uint32_t wb=rotator.slots[pending_rotator.slot].physical;
        uint32_t wb_site=READ_REG(lcd.Instance->WDMA_SITE);
        if (pending_matches(&descriptor,&y,&site) && wb_site>=wb &&
            wb_site-wb<=rotator.layout.bytes) {
            int result=player_rotator_written(&rotator,pending_rotator);
            if (result) { route_error=result; ++counters.rotator_errors; }
            else {
                /* Stop=1 is a single composer transaction, not a live chain. */
                lcd.CompData.comp_stopped=1;
                pending_writeback_us=(uint32_t)platform_time_us();
                pending_written=true; ++counters.writebacks; rotator_queue_ready();
            }
        } else { ++counters.mismatches; route_error=-EIO; }
    }
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
        if (!route_error && pending_valid && (!rotated || (pending_written &&
            player_rotator_display_ready(&rotator,pending_rotator)))) {
            /* SDK's debug ports expose the loaded descriptor, one word per
             * read. Reset the debug word counters, not the DMA engine. The
             * frame self-chain and layer source must both match this submit. */
            uint32_t descriptor, physical_y, site;
            bool matches=pending_matches(&descriptor,&physical_y,&site);
            counters.last_descriptor = descriptor;
            counters.last_physical_y = physical_y;
            counters.last_read_site = site;
            if (matches) {
                if (rotated) (void)player_rotator_displayed(&rotator,pending_rotator);
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

static int stop_rotator(void)
{
    if (!rotator_enabled) return 0;
    taskENTER_CRITICAL();
    rotator_stopping=true;
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_MASK),0);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CONTROL),PLAYER_ROTATOR_STOP);
    taskEXIT_CRITICAL();
    uint64_t deadline=platform_time_us()+100000u;
    bool acknowledged=false;
    for (;;) {
        taskENTER_CRITICAL();
        acknowledged=acknowledged || (READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS))&PLAYER_ROTATOR_ACK)!=0;
        rotator_service();
        uint32_t status=READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS));
        bool idle=!(status&PLAYER_ROTATOR_WORKING) &&
                  (acknowledged || (rotator.queued<0 && rotator.reading<0));
        taskEXIT_CRITICAL();
        if (idle) break;
        if (platform_time_us()>=deadline) return -ETIMEDOUT;
        vTaskDelay(1);
    }
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CONTROL),PLAYER_ROTATOR_STOP|PLAYER_ROTATOR_FLUSH);
    rotator_enabled=false; /* A previously latched IRQ cannot inspect reset metadata. */
    (void)player_rotator_reset(&rotator,true);
    return 0;
}

static void free_rotator(void)
{
    if (rotator_irq_registered) { ll_release_irq(IRQ_INTC0_ROTATE); rotator_irq_registered=false; }
    if (writeback_allocations[0]) CPM_GATE_Disable(CPM_Instance,CPM_CLKID_ROTATE);
    rotator_enabled=false;
    for (unsigned i=0;i<PLAYER_ROTATOR_SLOTS;++i) {
        free(writeback_allocations[i]); writeback_allocations[i]=NULL;
    }
}

static int start_rotator(void)
{
    player_rotator_layout_t geometry;
    int result=player_rotator_layout(1280,720,&geometry);
    if (result) return result;
    uint32_t physical[PLAYER_ROTATOR_SLOTS];
    for (unsigned i=0;i<PLAYER_ROTATOR_SLOTS;++i) {
        if (!writeback_allocations[i]) writeback_allocations[i]=malloc(geometry.bytes+511u);
        if (!writeback_allocations[i]) { free_rotator(); return -ENOMEM; }
        uintptr_t aligned=((uintptr_t)writeback_allocations[i]+511u)&~(uintptr_t)511u;
        /* Never touch these cached aliases again after producer ownership.
         * Both CUSTOM writeback and rotation are DMA operations. */
        CleanInvalidateDCache_by_Addr((unsigned long *)aligned,geometry.bytes);
        physical[i]=CPHYSADDR(aligned);
    }
    result=player_rotator_init(&rotator,1280,720,physical);
    if (result) { free_rotator(); return result; }
    rotator_event_count=0;
    rotator_has_error=false;
    pending_writeback_us=0;
    CPM_GATE_Enable(CPM_Instance,CPM_CLKID_ROTATE);
    SET_BIT(CPM_Instance->SRBC,1u<<22);
    vTaskDelay(1);
    CLEAR_BIT(CPM_Instance->SRBC,1u<<22);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_MASK),0);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CONTROL),PLAYER_ROTATOR_FLUSH);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CONTROL),0);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_CLEAR),0x1eu);
    (void)READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS));
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_FRAME_SIZE),geometry.frame_size);
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_GLOBAL),geometry.global);
    rotator_stopping=false;
    rotator_enabled=true;
    ll_request_irq(IRQ_INTC0_ROTATE,rotator_irq,NULL);
    rotator_irq_registered=true;
    WRITE_REG(ROT_REG(PLAYER_ROTATOR_MASK),PLAYER_ROTATOR_IRQS);
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
    WRITE_REG(lcd.Instance->DC_INTC, 0);
    /* The ROT display port needs the downstream DPU to finish its frame.
     * Stop the streaming producer before stopping that consumer. */
    int result = stop_rotator();
    if (result) return result; /* Hardware may still read the caller's buffers. */
    result=stop_dma();
    if (result) return result;
    panel_zc502_power_off();
    backlight_on = false;
    backlight_starting = false;
    free_rotator();
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
    rotated=false;
    route_phase=route_ack=0;
    route_error=0;
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

void player_display_gui_attach(bool attached)
{ taskENTER_CRITICAL(); gui_attached=attached; taskEXIT_CRITICAL(); }

int player_display_gui_mode(uint32_t *generation, bool *logical)
{
    if (!generation || !logical) return -EINVAL;
    taskENTER_CRITICAL();
    *generation=route_generation;
    *logical=route_phase ? requested_width==1280 : rotated;
    int result=route_error ? route_error : (int)route_phase;
    taskEXIT_CRITICAL();
    return result;
}

int player_display_gui_ack(uint32_t generation, unsigned phase)
{
    int result=0;
    taskENTER_CRITICAL();
    if (!gui_attached || generation!=route_generation || phase!=route_phase) result=-EINVAL;
    else route_ack=phase;
    taskEXIT_CRITICAL();
    return result;
}

uint32_t player_display_overlay_stride(void)
{ return rotated ? 1280u : 720u; }

int player_display_set_video_geometry(uint32_t width, uint32_t height)
{
    if (!initialized || !running) return -ENODEV;
    if (!((width==720 && height==1280) || (width==1280 && height==720))) return -EINVAL;
    if (route_error) return route_error;
    if (!route_phase) {
        if (layout.width==width && layout.height==height) return 0;
        if (scanout_valid || completion_valid || (pending_valid && pending_video_change)) return -EBUSY;
        taskENTER_CRITICAL();
        requested_width=width; requested_height=height;
        if (++route_generation==0) ++route_generation;
        route_phase=1; route_ack=gui_attached ? 0u : 1u;
        video_active=false;
        route_deadline=platform_time_us()+1000000u;
        taskEXIT_CRITICAL();
    }
    if (width!=requested_width || height!=requested_height) return -EBUSY;
    if (platform_time_us()>=route_deadline) return route_error=-ETIMEDOUT;
    if (route_phase==1) {
        if (route_ack!=1 || pending_valid || overlay_queued || overlay_completion_valid) return -EAGAIN;
        WRITE_REG(lcd.Instance->DC_INTC,0);
        int result=stop_rotator();
        if (!result) result=stop_dma();
        if (result) return route_error=result;
        if (width==1280) result=start_rotator();
        else free_rotator();
        if (result) return route_error=result;
        result=player_video_layout_nv12(width,height,&layout);
        if (result) return route_error=result;
        rotated=width==1280;
        for (unsigned i=0;i<2;++i) {
            layers[i].source_w=layers[i].scale_w=width;
            layers[i].source_h=layers[i].scale_h=height;
        }
        layers[0].stride=layers[0].uv_stride=layout.stride;
        layers[1].stride=layers[1].uv_stride=width;
        overlay_valid=false; scanout_argb=0;
        CLEAR_BIT(lcd.Instance->DC_COM_CONFIG,3u);
        if (rotated) SET_BIT(lcd.Instance->DC_COM_CONFIG,1u);
        WRITE_REG(lcd.Instance->DC_CLR_ST,0xffffffffu);
        WRITE_REG(lcd.Instance->DC_INTC,DC_DPU_INTC_EOD_MSK_0|DC_DPU_INTC_UOT_MSK_0|
                  (rotated ? DC_INTC_INT_EOW_0|DC_INTC_INT_OOW_0 : 0u));
        taskENTER_CRITICAL();
        route_phase=2; route_ack=gui_attached ? 0u : 2u;
        taskEXIT_CRITICAL();
    }
    if (route_ack!=2) return -EAGAIN;
    taskENTER_CRITICAL(); route_phase=route_ack=0; taskEXIT_CRITICAL();
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
    if (rotated) {
        int result=player_rotator_acquire(&rotator,&pending_rotator);
        if (result) return result;
    }
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
    /* HAL_COMP_Init mutates crop/scale/position in place. Never carry native
     * 720-wide GUI crop into a 1280-wide writeback: that enables a scaler and
     * its panel-width position clamp can underflow. Both sources already have
     * the composition geometry; request full-size pixels with no scaling. */
    for (unsigned i=0;i<2;++i) {
        layers[i].src_crop_x=layers[i].src_crop_y=0;
        layers[i].src_crop_w=layout.width; layers[i].src_crop_h=layout.height;
        layers[i].disp_pos_x=layers[i].disp_pos_y=0;
        layers[i].scale_w=layers[i].scale_h=0;
        layers[i].lay_scale_en=0;
    }
    if (HAL_COMP_Init(&lcd, layers) != HAL_OK) {
        if (rotated) (void)player_rotator_cancel_write(&rotator,pending_rotator);
        return -EIO;
    }
    pending_descriptor = CPHYSADDR(&lcd.CompData.FrameDesc[lcd.CompData.comp_next_frame]);
    WRITE_REG(lcd.Instance->DC_CLR_ST, DC_INTC_INT_EOD_0);
    pending_started_us = platform_time_us();
    pending_written=false;
    pending_valid = true;
    HAL_StatusTypeDef started=HAL_OK;
    if (rotated) {
        unsigned index=lcd.CompData.comp_next_frame;
        uint32_t *words=(uint32_t *)&lcd.CompData.FrameDesc[index];
        words[1]=rotator.layout.frame_size;
        words[2]=(7u<<16)|3u; /* CUSTOM, one-shot, WB enabled, direct disabled. */
        words[3]=rotator.slots[pending_rotator.slot].physical;
        words[4]=rotator.layout.stride;
        CleanDCache_by_Addr((unsigned long *)words,sizeof(lcd.CompData.FrameDesc[0]));
        CleanDCache_by_Addr((unsigned long *)lcd.CompData.LayerDesc[index],sizeof(lcd.CompData.LayerDesc[0]));
        /* SDK HAL_COMP_Start clears ROT_SEL; keep its start sequence local. */
        WRITE_REG(lcd.Instance->DC_COM_CONFIG,(READ_REG(lcd.Instance->DC_COM_CONFIG)&~3u)|1u);
        WRITE_REG(lcd.Instance->DC_FRM_CFG_ADDR,pending_descriptor);
        WRITE_REG(lcd.Instance->DC_FRM_CFG_CTRL,1u);
        lcd.CompData.comp_stopped=0;
    } else started=HAL_COMP_Start(&lcd);
    if (started == HAL_OK) {
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
    if (route_error) return route_error;
    if (route_phase) return -EAGAIN;
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
    if (route_error) return route_error;
    if (route_phase) return -EAGAIN;
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
    if (route_error) return route_error;
    if (route_phase) return -EAGAIN;
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
    if (route_error) return route_error;
    int queued_result = 0;
    taskENTER_CRITICAL();
    queued_result=resolve_rotator_start();
    /* With paused/no video, GUI polling itself drives the queued update. */
    if (!queued_result && overlay_queued && !pending_valid && !video_active)
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
    if (route_error) return route_error;
    int result = 0;
    taskENTER_CRITICAL();
    result=resolve_rotator_start();
    if (!result && completion_valid) {
        *completion = completed;
        result = 1;
    }
    taskEXIT_CRITICAL();
    if (result<0) return result;
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
    stats->width=layout.width; stats->height=layout.height;
    stats->route_generation=route_generation; stats->route_phase=route_phase;
    stats->route_error=route_error;
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
    printf("DISPLAY_ROUTE width=%lu height=%lu rotated=%u generation=%lu phase=%lu error=%d writebacks=%lu rot_sof=%lu rot_eof=%lu repeats=%lu rot_errors=%lu held_sof=%lu late_binds=%lu\n",
           (unsigned long)timing.width,(unsigned long)timing.height,(unsigned)rotated,
           (unsigned long)timing.route_generation,(unsigned long)timing.route_phase,timing.route_error,
           (unsigned long)timing.writebacks,(unsigned long)timing.rotator_sof,
           (unsigned long)timing.rotator_eof,(unsigned long)timing.rotator_repeats,(unsigned long)timing.rotator_errors,
           (unsigned long)timing.rotator_held_sof,(unsigned long)timing.rotator_late_binds);
    if (rotator_enabled) {
        rotator_event_t events[16], first;
        unsigned count;
        bool failed;
        taskENTER_CRITICAL();
        count=rotator_event_count;
        memcpy(events,rotator_events,count*sizeof(*events));
        failed=rotator_has_error; first=rotator_first_error;
        taskEXIT_CRITICAL();
        if (failed)
            printf("DISPLAY_ROT_FIRST_ERROR status=%08lx flags=%02lx site=%08lx queued=%d reading=%d last=%d latest=%d result=%d us=%lu eod_seq=%lu wb_us=%lu kind=%lu\n",
                   (unsigned long)first.status,(unsigned long)(first.status&0x1eu),(unsigned long)first.site,
                   first.queued,first.reading,first.last,first.latest,first.result,
                   (unsigned long)first.time_us,(unsigned long)first.eod_sequence,
                   (unsigned long)first.writeback_us,(unsigned long)first.kind);
        for (unsigned i=0;i<count;++i)
            printf("DISPLAY_ROT_EVENT index=%u status=%08lx flags=%02lx site=%08lx queued=%d reading=%d last=%d latest=%d result=%d us=%lu eod_seq=%lu wb_us=%lu kind=%lu\n",
                   i,(unsigned long)events[i].status,(unsigned long)(events[i].status&0x1eu),(unsigned long)events[i].site,
                   events[i].queued,events[i].reading,events[i].last,events[i].latest,events[i].result,
                   (unsigned long)events[i].time_us,(unsigned long)events[i].eod_sequence,
                   (unsigned long)events[i].writeback_us,(unsigned long)events[i].kind);
        printf("DISPLAY_ROT status=%08lx global=%08lx frame=%08lx mask=%08lx site=%08lx queued=%d reading=%d last=%d latest=%d\n",
               READ_REG(ROT_REG(PLAYER_ROTATOR_STATUS)),READ_REG(ROT_REG(PLAYER_ROTATOR_GLOBAL)),
               READ_REG(ROT_REG(PLAYER_ROTATOR_FRAME_SIZE)),READ_REG(ROT_REG(PLAYER_ROTATOR_MASK)),
               READ_REG(ROT_REG(PLAYER_ROTATOR_READ_SITE)),rotator.queued,rotator.reading,
               rotator.last,rotator.latest_started);
        for (unsigned i=0;i<PLAYER_ROTATOR_SLOTS;++i)
            printf("DISPLAY_WB slot=%u physical=%08lx flags=%02lx sequence=%lu\n",i,
                   (unsigned long)rotator.slots[i].physical,(unsigned long)rotator.slots[i].flags,
                   (unsigned long)rotator.slots[i].sequence);
    }
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
    if (clocks_enabled) {
        uint32_t frame_words[16], video_words[16], ui_words[16], wb_site;
        taskENTER_CRITICAL();
        WRITE_REG(lcd.Instance->DC_CTRL,DC_CTRL_DES_CNT_RST_0);
        for (unsigned i=0;i<16;++i) {
            frame_words[i]=READ_REG(lcd.Instance->FRM_DES_READ);
            video_words[i]=READ_REG(lcd.Instance->LAY0_DES_READ);
            ui_words[i]=READ_REG(lcd.Instance->LAY1_DES_READ);
        }
        wb_site=READ_REG(lcd.Instance->WDMA_SITE);
        taskEXIT_CRITICAL();
        printf("DISPLAY_WDMA site=%08lx comp_stopped=%d pending_descriptor=%08lx pending_y=%08lx pending_argb=%08lx pending_written=%u\n",
               (unsigned long)wb_site,lcd.CompData.comp_stopped,(unsigned long)pending_descriptor,
               (unsigned long)pending_y,(unsigned long)pending_argb,(unsigned)pending_written);
        for (unsigned i=0;i<16;++i)
            printf("DISPLAY_LOADED %u frame=%08lx layer0=%08lx layer1=%08lx\n",i,
                   (unsigned long)frame_words[i],(unsigned long)video_words[i],(unsigned long)ui_words[i]);
    }
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
