#include "player_display.h"
#include "player_rotator.h"
#include "x2600_hal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

DPU_TypeDef fake_dpu;
DSI_TypeDef fake_dsi;
CPM_TypeDef fake_cpm;
GPIO_TypeDef fake_gpio;
PWM_TypeDef fake_pwm;
volatile unsigned long fake_rotator_regs[17];
struct lcd_data panel_zc502_data;
int fake_working;
static struct lay_cfg submitted_layers[4];
static uint32_t next_descriptor, loaded_descriptor, loaded_y, loaded_ui;
static unsigned word, starts, backlights;
static uint64_t clock_us;
static void (*lcd_irq)(int, void *);
static void (*rot_irq)(int,void *);
static bool rot_hang;
static uintptr_t cached_writebacks[3];
static uint32_t queued_rotation;
static LCD_HandleTypeDef *last_lcd;

void fake_cache_publish(unsigned long *pointer, long bytes)
{
    if (bytes!=3686400) return;
    for (unsigned i=0;i<3;++i) if (cached_writebacks[i]==(uintptr_t)pointer) return;
    for (unsigned i=0;i<3;++i) if (!cached_writebacks[i]) { cached_writebacks[i]=(uintptr_t)pointer; return; }
    assert(!"unexpected fourth writeback allocation");
}
uint32_t fake_physical(uintptr_t p)
{
    for (unsigned i=0;i<3;++i) if (cached_writebacks[i] && p==cached_writebacks[i]) return 0x03000000u+i*0x00400000u;
    return (uint32_t)(p&0x03ffffffu);
}

unsigned long fake_read(const volatile unsigned long *reg)
{
    if (reg == &fake_dpu.FRM_DES_READ) return loaded_descriptor;
    if (reg == &fake_dpu.LAY0_DES_READ) return word++ == 2 ? loaded_y : 0;
    if (reg == &fake_dpu.LAY1_DES_READ) return word++ == 2 ? loaded_ui : 0;
    return *reg;
}
void fake_write(volatile unsigned long *reg, unsigned long value)
{
    *reg = value;
    if (reg == &fake_dpu.DC_CTRL) word = 0;
    if (reg == &fake_dpu.DC_CLR_ST) fake_dpu.DC_INT_FLAG &= ~value;
    if (reg == &fake_dpu.DC_FRM_CFG_CTRL && value==1) {
        next_descriptor=(uint32_t)fake_dpu.DC_FRM_CFG_ADDR; ++starts;
    }
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_CLEAR/4])
        fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]&=~value;
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_BUFFER/4]) queued_rotation=(uint32_t)value&~1u;
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_CONTROL/4] && (value&PLAYER_ROTATOR_STOP) && !rot_hang)
        fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=PLAYER_ROTATOR_ACK;
}
uint64_t platform_time_us(void) { return clock_us; }
void vTaskDelay(unsigned ticks) { clock_us += ticks * 1000u; }
void ll_request_irq(int irq, void (*handler)(int, void *), void *argument)
{ (void)argument; if (irq==IRQ_INTC0_ROTATE) rot_irq=handler; else lcd_irq=handler; }
void ll_release_irq(int irq) { if (irq==IRQ_INTC0_ROTATE) rot_irq=NULL; else lcd_irq=NULL; }
int panel_zc502_result(void) { return 0; }
void panel_zc502_backlight_on(void) { ++backlights; }
void panel_zc502_power_off(void) { }
HAL_StatusTypeDef HAL_LCDC_Init(LCD_HandleTypeDef *lcd) { (void)lcd; return HAL_OK; }
HAL_StatusTypeDef HAL_LCDC_Enable(LCD_HandleTypeDef *lcd) { (void)lcd; return HAL_OK; }
HAL_StatusTypeDef HAL_COMP_Stop(LCD_HandleTypeDef *lcd, int mode)
{ (void)lcd; (void)mode; return HAL_OK; }
HAL_StatusTypeDef HAL_SRDMA_Stop(LCD_HandleTypeDef *lcd, int mode)
{ (void)lcd; (void)mode; return HAL_OK; }
HAL_StatusTypeDef HAL_COMP_Init(LCD_HandleTypeDef *lcd, struct lay_cfg *layers)
{
    last_lcd=lcd;
    if (lcd->CompData.comp_stopped) lcd->CompData.comp_next_frame = 0;
    else lcd->CompData.comp_next_frame = (lcd->CompData.comp_next_frame + 1) % 3u;
    memcpy(submitted_layers, layers, sizeof(submitted_layers));
    lcd->CompData.FrameDesc[lcd->CompData.comp_next_frame].words[0]=
        CPHYSADDR(&lcd->CompData.FrameDesc[lcd->CompData.comp_next_frame]);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_COMP_Start(LCD_HandleTypeDef *lcd)
{
    next_descriptor = CPHYSADDR(&lcd->CompData.FrameDesc[lcd->CompData.comp_next_frame]);
    lcd->CompData.comp_stopped = 0;
    ++starts;
    return HAL_OK;
}
static void interrupt_only(void)
{
    assert(lcd_irq);
    fake_dpu.DC_INT_FLAG = DC_INTC_INT_EOD_0;
    lcd_irq(IRQ_INTC0_LCD, NULL);
}
static void latch(void)
{
    loaded_descriptor = next_descriptor;
    loaded_y = submitted_layers[0].lay_en ? submitted_layers[0].addr : 0;
    loaded_ui = submitted_layers[1].lay_en ? submitted_layers[1].addr : 0;
    fake_dpu.LAY0_SITE = loaded_y ? loaded_y + 64u : 0;
    fake_dpu.LAY1_SITE = loaded_ui ? loaded_ui + 64u : 0;
    clock_us += 33333;
    interrupt_only();
}
static player_display_frame_t frame(unsigned slot)
{
    player_display_frame_t result = {
        (void *)(uintptr_t)(0x81000000u + slot * 0x00200000u),
        1474560, {1, 1, slot}
    };
    return result;
}

static void continuous_video_priority(void)
{
    assert(player_display_init() == 0 && player_display_start() == 0);
    player_display_set_video_active(true);
    player_display_frame_t a = frame(0), b = frame(1), c = frame(2);
    const uint32_t *ui_a = (const uint32_t *)(uintptr_t)0x82000000u;
    const uint32_t *ui_b = (const uint32_t *)(uintptr_t)0x82400000u;
    player_display_completion_t completion;
    uint32_t serial;
    unsigned baseline = starts;
    assert(player_display_submit_frame(&a) == 0);
    assert(player_display_submit_overlay(ui_a, PLAYER_DISPLAY_OVERLAY_BYTES, 20) == 0);
    latch();
    /* GUI runs first after EOD, before the media worker consumes completion:
     * this used to start a GUI-only transaction and steal the next refresh. */
    assert(player_display_poll_overlay_completion(&serial) == 0 && starts == baseline + 1u);
    assert(player_display_submit_frame(&b) == -EAGAIN);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 0);
    assert(player_display_poll_overlay_completion(&serial) == 0 && starts == baseline + 1u);
    assert(player_display_submit_frame(&b) == 0 && starts == baseline + 2u);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 20);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 1);
    /* Even an idle hardware slot is reserved for the next continuous video.
     * GUI polling cannot turn periodic diagnostic redraws into extra EODs. */
    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 21) == 0);
    for (unsigned i = 0; i < 10; ++i) {
        clock_us += 1000u;
        assert(player_display_poll_overlay_completion(&serial) == 0 && starts == baseline + 2u);
    }
    assert(player_display_submit_frame(&c) == 0 && starts == baseline + 3u);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 21);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 2);
    player_display_set_video_active(false);
    assert(player_display_submit_overlay(ui_a, PLAYER_DISPLAY_OVERLAY_BYTES, 22) == 0);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 22);
    assert(player_display_poll_completion(&completion) == 0);
    /* A request already queued when PAUSE/ENDED arrives is also released. */
    player_display_set_video_active(true);
    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 23) == 0);
    player_display_set_video_active(false);
    assert(player_display_poll_overlay_completion(&serial) == 0 && starts == baseline + 5u);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 23);
    player_display_stats_t stats;
    player_display_get_stats(&stats);
    assert(stats.video_submitted == 3 && stats.video_busy == 1 && stats.confirmed == 3);
    assert(stats.overlay_submitted == 4 && stats.overlay_queued_requests == 3);
    assert(stats.overlay_merged == 2 && stats.overlay_only == 2 && stats.overlay_confirmed == 4);
    assert(stats.eod_period_min_us == 33333 && stats.eod_period_max_us == 43333);
    assert(stats.video_latency_max_us == 33333 && !stats.video_active);
    assert(player_display_stop() == 0);
}

static void writeback(void)
{
    loaded_descriptor=next_descriptor;
    loaded_y=submitted_layers[0].lay_en ? submitted_layers[0].addr : 0;
    loaded_ui=submitted_layers[1].lay_en ? submitted_layers[1].addr : 0;
    fake_dpu.LAY0_SITE=loaded_y+64u;
    fake_dpu.LAY1_SITE=loaded_ui+64u;
    uint32_t *words=last_lcd->CompData.FrameDesc[last_lcd->CompData.comp_next_frame].words;
    assert(words[1]==0x02d00500 && words[2]==0x00070003 && words[4]==1280);
    fake_dpu.WDMA_SITE=words[3]+64u;
    fake_dpu.DC_INT_FLAG=DC_INTC_INT_EOW_0;
    lcd_irq(IRQ_INTC0_LCD,NULL);
}

static void rotation_event(uint32_t flags)
{
    assert(rot_irq);
    if (flags&PLAYER_ROTATOR_SOF) fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=queued_rotation+64u;
    fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=flags;
    rot_irq(IRQ_INTC0_ROTATE,NULL);
}

static void wide_route(void)
{
    assert(player_display_init()==0 && player_display_start()==0);
    player_display_gui_attach(true);
    assert(player_display_set_video_geometry(1279,720)==-EINVAL);
    assert(player_display_set_video_geometry(1280,720)==-EAGAIN);
    uint32_t generation=0,serial=0;
    bool logical=false;
    assert(player_display_gui_mode(&generation,&logical)==1 && logical);
    assert(player_display_gui_ack(generation+1u,1)==-EINVAL);
    assert(player_display_gui_ack(generation,1)==0);
    assert(player_display_set_video_geometry(1280,720)==-EAGAIN);
    assert(player_display_gui_mode(&generation,&logical)==2 && logical);
    assert(fake_rotator_regs[PLAYER_ROTATOR_GLOBAL/4]==0x03000010);
    assert(fake_rotator_regs[PLAYER_ROTATOR_FRAME_SIZE/4]==0x02d00500);
    assert(player_display_gui_ack(generation,2)==0);
    assert(player_display_set_video_geometry(1280,720)==0);
    assert(player_display_overlay_stride()==1280);
    player_display_frame_t a=frame(0),b=frame(1);
    a.capacity=b.capacity=1382400;
    player_display_completion_t c;
    assert(player_display_submit_frame(&a)==0);
    assert(submitted_layers[0].source_w==1280 && submitted_layers[0].source_h==720);
    assert(submitted_layers[0].stride==1280 && fake_dpu.DC_COM_CONFIG==1);
    interrupt_only();
    assert(player_display_poll_completion(&c)==0); /* EOD before EOW/SOF is old scanout. */
    writeback();
    assert(player_display_poll_completion(&c)==0); /* EOW cannot claim a displayed video. */
    rotation_event(1u<<4);
    interrupt_only();
    assert(player_display_poll_completion(&c)==0); /* FETCH_ADDR cannot claim it either. */
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    assert(player_display_poll_completion(&c)==0);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_completion(&c)==1 && c.has_latched && c.latched.slot==0);
    assert(player_display_set_video_geometry(720,1280)==-EBUSY);
    assert(player_display_submit_frame(&b)==0);
    writeback();
    rotation_event(PLAYER_ROTATOR_EOF);
    interrupt_only();
    assert(player_display_poll_completion(&c)==0); /* EOF still names frame a. */
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_completion(&c)==1 && c.has_latched && c.latched.slot==1);
    assert(c.has_retired && c.retired.slot==0);
    assert(player_display_submit_overlay((const uint32_t *)(uintptr_t)0x82000000u,
           PLAYER_DISPLAY_OVERLAY_BYTES,99)==0);
    writeback();
    rotation_event(PLAYER_ROTATOR_EOF|PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_overlay_completion(&serial)==1 && serial==99);
    assert(player_display_poll_completion(&c)==0); /* GUI-only is not another video. */
    rotation_event(PLAYER_ROTATOR_EOF); /* END/PAUSE repeats the held CUSTOM frame. */
    player_display_stats_t stats;
    player_display_get_stats(&stats);
    assert(stats.confirmed==2 && stats.writebacks==3 && stats.rotator_repeats==1);
    assert(player_display_clear_video()==0);
    writeback();
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING); /* Repeat drains first. */
    rotation_event(PLAYER_ROTATOR_EOF);
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_completion(&c)==1 && !c.has_latched && c.has_retired && c.retired.slot==1);
    assert(player_display_set_video_geometry(720,1280)==-EAGAIN);
    assert(player_display_gui_mode(&generation,&logical)==1 && !logical);
    assert(player_display_gui_ack(generation,1)==0);
    assert(player_display_set_video_geometry(720,1280)==-EAGAIN);
    assert(player_display_gui_mode(&generation,&logical)==2 && !logical);
    assert(player_display_gui_ack(generation,2)==0);
    assert(player_display_set_video_geometry(720,1280)==0 && !rot_irq);
    assert(player_display_overlay_stride()==720);
    a=frame(0);
    assert(player_display_submit_frame(&a)==0); latch();
    assert(player_display_poll_completion(&c)==1 && c.has_latched);
    player_display_gui_attach(false);
    assert(player_display_stop()==0);
    memset(cached_writebacks,0,sizeof(cached_writebacks));
    assert(player_display_init()==0 && player_display_start()==0);
    assert(player_display_set_video_geometry(1280,720)==0);
    a.capacity=1382400;
    assert(player_display_submit_frame(&a)==0); writeback();
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    rot_hang=true;
    assert(player_display_stop()==-ETIMEDOUT && rot_irq); /* All CUSTOM allocations stay pinned. */
    rot_hang=false;
    assert(player_display_stop()==0 && !rot_irq);
}

int main(void)
{
    assert(player_display_init() == 0 && player_display_start() == 0);
    player_display_frame_t a = frame(0), b = frame(1), c = frame(2);
    const uint32_t *ui_a = (const uint32_t *)(uintptr_t)0x82000000u;
    const uint32_t *ui_b = (const uint32_t *)(uintptr_t)0x82400000u;
    player_display_completion_t completion;
    uint32_t serial;
    assert(player_display_submit_frame(&a) == 0 && starts == 1);
    assert(player_display_submit_overlay(ui_a, PLAYER_DISPLAY_OVERLAY_BYTES, 10) == 0);
    assert(starts == 1); /* Accepted and pinned behind video, not overwritten. */
    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 11) == -EAGAIN);
    interrupt_only(); /* EOD with the old descriptor must not retire anything. */
    assert(player_display_poll_completion(&completion) == 0);
    latch();
    assert(player_display_poll_completion(&completion) == 1 && completion.has_latched);
    assert(completion.latched.slot == 0 && !completion.has_retired);
    assert(player_display_submit_frame(&b) == 0 && starts == 2);
    assert(submitted_layers[0].lay_en && submitted_layers[1].lay_en);
    assert(submitted_layers[0].lay_z_order == 2 && submitted_layers[1].lay_z_order == 3);
    assert(submitted_layers[2].lay_z_order == 1 && submitted_layers[3].lay_z_order == 0);
    assert(submitted_layers[1].domain_multi == 0 && !submitted_layers[1].g_alpha_en);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 10);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 1);
    assert(completion.has_retired && completion.retired.slot == 0 && backlights == 1);

    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 11) == 0);
    assert(player_display_poll_completion(&completion) == 0);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 11);
    assert(player_display_poll_completion(&completion) == 0); /* GUI cannot fake a video latch. */
    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 12) == -EINVAL);

    assert(player_display_submit_frame(&c) == 0);
    assert(player_display_submit_overlay(ui_a, PLAYER_DISPLAY_OVERLAY_BYTES, 12) == 0);
    unsigned before = starts;
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 0 && starts == before + 1);
    /* GUI polling drives a queued update without another video submission. */
    assert(player_display_clear_video() == -EAGAIN);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 12);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 2);
    assert(completion.has_retired && completion.retired.slot == 1);
    assert(player_display_poll_completion(&completion) == 0);

    assert(player_display_clear_video() == 0 && !submitted_layers[0].lay_en && submitted_layers[1].lay_en);
    latch();
    assert(player_display_poll_completion(&completion) == 1 && !completion.has_latched);
    assert(completion.has_retired && completion.retired.slot == 2);
    player_display_stats_t stats;
    player_display_get_stats(&stats);
    assert(!stats.pending && !stats.scanout && stats.overlay_scanout);
    assert(stats.confirmed == 3 && stats.overlay_confirmed == 3 && stats.mismatches == 1);
    fake_working = 1;
    assert(player_display_stop() == -ETIMEDOUT);
    assert(player_display_start() == -ENODEV);
    fake_working = 0;
    assert(player_display_stop() == 0);
    assert(player_display_poll_overlay_completion(&serial) == 0);
    continuous_video_priority();
    wide_route();
    puts("PASS production display: legacy guards/priority, CUSTOM EOW/SOF/EOD ownership, paused GUI-only/repeat, GUI route ACK, native compatibility, stop pin");
    return 0;
}
