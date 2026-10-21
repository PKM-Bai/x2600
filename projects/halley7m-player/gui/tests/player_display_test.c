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
static unsigned word, starts, descriptor_posts, channel_writes, backlights;
static uintptr_t published_frame, published_layers;
static uint64_t clock_us;
static void (*lcd_irq)(int, void *);
static void (*rot_irq)(int,void *);
static bool rot_hang;
static bool composer_started;
static bool display_consumer_active;
static uintptr_t cached_writebacks[3];
static uint32_t queued_rotation;
static uint32_t posted_rotator_clear;
static LCD_HandleTypeDef *last_lcd;

void fake_cache_publish(unsigned long *pointer, long bytes)
{
    if (bytes==64) published_frame=(uintptr_t)pointer;
    if (bytes==256) published_layers=(uintptr_t)pointer;
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
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_STATUS/4] && posted_rotator_clear) {
        unsigned long captured=*reg;
        fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]&=~posted_rotator_clear;
        posted_rotator_clear=0;
        return captured; /* A status read drains a posted W1C write. */
    }
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
    if (reg == &fake_dpu.DC_COM_CONFIG) ++channel_writes;
    if (reg == &fake_dpu.DC_FRM_CFG_ADDR) {
        unsigned index=last_lcd->CompData.comp_next_frame;
        assert(value==CPHYSADDR(&last_lcd->CompData.FrameDesc[index]));
        assert(published_frame==(uintptr_t)&last_lcd->CompData.FrameDesc[index]);
        assert(published_layers==(uintptr_t)last_lcd->CompData.LayerDesc[index]);
        published_frame=published_layers=0;
        next_descriptor=(uint32_t)value;
        ++descriptor_posts;
    }
    if (reg == &fake_dpu.DC_FRM_CFG_CTRL && value==1) {
        assert(!composer_started);
        composer_started=true;
        display_consumer_active=true;
        next_descriptor=(uint32_t)fake_dpu.DC_FRM_CFG_ADDR; ++starts;
    }
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_CLEAR/4])
        posted_rotator_clear|=(uint32_t)value;
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_BUFFER/4]) queued_rotation=(uint32_t)value&~1u;
    if (reg==&fake_rotator_regs[PLAYER_ROTATOR_CONTROL/4] && (value&PLAYER_ROTATOR_STOP) && !rot_hang &&
        (display_consumer_active || !(fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]&PLAYER_ROTATOR_WORKING)))
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
{ (void)lcd; (void)mode; composer_started=false; display_consumer_active=false; return HAL_OK; }
HAL_StatusTypeDef HAL_SRDMA_Stop(LCD_HandleTypeDef *lcd, int mode)
{ (void)lcd; (void)mode; return HAL_OK; }
HAL_StatusTypeDef HAL_COMP_Init(LCD_HandleTypeDef *lcd, struct lay_cfg *layers)
{
    last_lcd=lcd;
    if (lcd->CompData.comp_stopped) lcd->CompData.comp_next_frame = 0;
    else lcd->CompData.comp_next_frame = (lcd->CompData.comp_next_frame + 1) % 3u;
    /* The real SDK mutates this configuration, including persistent crop.
     * Its position limit uses native panel dimensions even in WB mode. */
    for (unsigned i=0;i<4;++i) {
        struct lay_cfg *l=&layers[i];
        if (!l->lay_en) { l->scale_w=l->scale_h=l->lay_scale_en=0; continue; }
        if (!l->src_crop_w || l->src_crop_w>=l->source_w) l->src_crop_w=l->source_w;
        if (!l->src_crop_h || l->src_crop_h>=l->source_h) l->src_crop_h=l->source_h;
        l->lay_scale_en=(l->src_crop_w!=l->scale_w || l->src_crop_h!=l->scale_h) && l->scale_w && l->scale_h;
        if (!l->scale_w || !l->scale_h) l->scale_w=l->scale_h=0;
        l->disp_pos_x=l->disp_pos_x+l->scale_w>=720u ? 720u-l->scale_w : l->disp_pos_x;
        l->disp_pos_y=l->disp_pos_y+l->scale_h>=1280u ? 1280u-l->scale_h : l->disp_pos_y;
    }
    memcpy(submitted_layers, layers, sizeof(submitted_layers));
    lcd->CompData.FrameDesc[lcd->CompData.comp_next_frame].words[0]=
        CPHYSADDR(&lcd->CompData.FrameDesc[lcd->CompData.comp_next_frame]);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_COMP_Start(LCD_HandleTypeDef *lcd)
{
    /* Model the real HAL's publication and MMIO so the regression distinguishes
     * the initial START from address-only insertion into a running chain. */
    unsigned index=lcd->CompData.comp_next_frame;
    WRITE_REG(lcd->Instance->DC_COM_CONFIG,READ_REG(lcd->Instance->DC_COM_CONFIG)&~3u);
    fake_cache_publish((unsigned long *)&lcd->CompData.FrameDesc[index],sizeof(lcd->CompData.FrameDesc[0]));
    fake_cache_publish((unsigned long *)lcd->CompData.LayerDesc[index],sizeof(lcd->CompData.LayerDesc[0]));
    WRITE_REG(lcd->Instance->DC_FRM_CFG_ADDR,CPHYSADDR(&lcd->CompData.FrameDesc[index]));
    WRITE_REG(lcd->Instance->DC_FRM_CFG_CTRL,1u);
    lcd->CompData.comp_stopped = 0;
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
    loaded_y = submitted_layers[0].lay_en ?
               last_lcd->CompData.LayerDesc[last_lcd->CompData.comp_next_frame][0].words[2] : 0;
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
    unsigned baseline = descriptor_posts;
    assert(player_display_submit_frame(&a) == 0);
    assert(player_display_submit_overlay(ui_a, PLAYER_DISPLAY_OVERLAY_BYTES, 20) == 0);
    latch();
    /* GUI runs first after EOD, before the media worker consumes completion:
     * this used to start a GUI-only transaction and steal the next refresh. */
    assert(player_display_poll_overlay_completion(&serial) == 0 && descriptor_posts == baseline + 1u);
    assert(player_display_submit_frame(&b) == -EAGAIN);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 0);
    assert(player_display_poll_overlay_completion(&serial) == 0 && descriptor_posts == baseline + 1u);
    assert(player_display_submit_frame(&b) == 0 && descriptor_posts == baseline + 2u);
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 1 && serial == 20);
    assert(player_display_poll_completion(&completion) == 1 && completion.latched.slot == 1);
    /* Even an idle hardware slot is reserved for the next continuous video.
     * GUI polling cannot turn periodic diagnostic redraws into extra EODs. */
    assert(player_display_submit_overlay(ui_b, PLAYER_DISPLAY_OVERLAY_BYTES, 21) == 0);
    for (unsigned i = 0; i < 10; ++i) {
        clock_us += 1000u;
        assert(player_display_poll_overlay_completion(&serial) == 0 && descriptor_posts == baseline + 2u);
    }
    assert(player_display_submit_frame(&c) == 0 && descriptor_posts == baseline + 3u);
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
    assert(player_display_poll_overlay_completion(&serial) == 0 && descriptor_posts == baseline + 5u);
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
    assert(composer_started); /* A second one-shot needs a second actual start. */
    composer_started=false;
    loaded_descriptor=next_descriptor;
    loaded_y=submitted_layers[0].lay_en ? submitted_layers[0].addr : 0;
    loaded_ui=submitted_layers[1].lay_en ? submitted_layers[1].addr : 0;
    fake_dpu.LAY0_SITE=loaded_y+64u;
    fake_dpu.LAY1_SITE=loaded_ui+64u;
    uint32_t *words=last_lcd->CompData.FrameDesc[last_lcd->CompData.comp_next_frame].words;
    assert(words[1]==0x02d00500 && words[2]==0x00070003 && words[4]==1280);
    for (unsigned i=0;i<2;++i) if (submitted_layers[i].lay_en) {
        assert(submitted_layers[i].src_crop_w==1280 && submitted_layers[i].src_crop_h==720);
        assert(!submitted_layers[i].lay_scale_en && !submitted_layers[i].scale_w && !submitted_layers[i].scale_h);
        assert(!submitted_layers[i].disp_pos_x && !submitted_layers[i].disp_pos_y);
    }
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
    uint32_t native_serial=0;
    assert(player_display_submit_overlay((const uint32_t *)(uintptr_t)0x82000000u,
           PLAYER_DISPLAY_OVERLAY_BYTES,80)==0);
    latch();
    assert(player_display_poll_overlay_completion(&native_serial)==1 && native_serial==80);
    assert(submitted_layers[1].src_crop_w==720 && submitted_layers[1].src_crop_h==1280);
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
    unsigned wide_starts=starts;
    assert(player_display_submit_frame(&a)==0 && starts==wide_starts+1u);
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
    assert(!(fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]&0x1eu));
    /* First-board regression hypothesis: one CUSTOM writeback, then a held
     * frame's combined EOF/SOF, without another software command. */
    rotation_event(PLAYER_ROTATOR_EOF|PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING|0x000c0080u);
    assert(player_display_poll_completion(&c)==0);
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING|0x000c0080u);
    player_display_stats_t held;
    player_display_get_stats(&held);
    assert(!held.route_error && !held.rotator_errors && held.rotator_held_sof==2);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_completion(&c)==1 && c.has_latched && c.latched.slot==0);
    /* Real R2: the first video WB succeeds, then a GUI-only one-shot must also
     * write back after native GUI has previously mutated its crop to 720. */
    assert(player_display_submit_overlay((const uint32_t *)(uintptr_t)0x82400000u,
           PLAYER_DISPLAY_OVERLAY_BYTES,81)==0);
    writeback();
    rotation_event(PLAYER_ROTATOR_EOF|PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_overlay_completion(&serial)==1 && serial==81);
    assert(player_display_poll_completion(&c)==0);
    assert(player_display_set_video_geometry(720,1280)==-EBUSY);
    assert(player_display_submit_frame(&b)==0);
    uint32_t old_rotation=queued_rotation;
    writeback();
    uint32_t new_rotation=queued_rotation;
    assert(old_rotation!=new_rotation);
    fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=old_rotation+128u;
    fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING;
    rot_irq(IRQ_INTC0_ROTATE,NULL); /* A delayed/held old SOF leaves the new address queued. */
    assert(player_display_poll_completion(&c)==0);
    fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=PLAYER_ROTATOR_FETCH|PLAYER_ROTATOR_WORKING;
    rot_irq(IRQ_INTC0_ROTATE,NULL);
    fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=new_rotation+64u;
    assert(player_display_poll_completion(&c)==0); /* FETCH without SOF cannot bind/latch. */
    fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=old_rotation+128u;
    fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=PLAYER_ROTATOR_EOF|PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING;
    rot_irq(IRQ_INTC0_ROTATE,NULL); /* Actual new SOF still reports the old debug site. */
    interrupt_only();
    assert(player_display_poll_completion(&c)==0); /* Old site remains pinned and unpresented. */
    fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=new_rotation+64u;
    assert(player_display_poll_completion(&c)==0); /* Site binds, but still no new EOD. */
    clock_us+=33033; interrupt_only();
    assert(player_display_poll_completion(&c)==1 && c.has_latched && c.latched.slot==1);
    assert(c.has_retired && c.retired.slot==0);
    player_display_get_stats(&held);
    assert(held.rotator_late_binds==1 && !held.route_error);
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
    assert(stats.confirmed==2 && stats.writebacks==4 && stats.rotator_repeats==1);
    assert(starts==wide_starts+4u); /* Every completed one-shot was started. */
    assert(player_display_clear_video()==0);
    assert(starts==wide_starts+5u);
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
    assert(player_display_submit_frame(&a)==0 && starts==wide_starts+6u); latch();
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
    memset(cached_writebacks,0,sizeof(cached_writebacks));
    assert(player_display_init()==0 && player_display_start()==0);
    assert(player_display_set_video_geometry(1280,720)==0);
    assert(player_display_submit_frame(&a)==0); writeback();
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    fake_rotator_regs[PLAYER_ROTATOR_READ_SITE/4]=queued_rotation+3686400u;
    fake_rotator_regs[PLAYER_ROTATOR_STATUS/4]=PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING;
    rot_irq(IRQ_INTC0_ROTATE,NULL);
    assert(player_display_poll_completion(&c)<0); /* Unknown/reused address remains pinned. */
    player_display_get_stats(&stats);
    assert(stats.route_error && stats.rotator_errors==1 && stats.pending && !stats.scanout);
    assert(player_display_stop()==0);
}

static void scaled_format_route(void)
{
    memset(cached_writebacks,0,sizeof(cached_writebacks));
    assert(player_display_init()==0 && player_display_start()==0);
    player_video_format_t f={640,480,640,480,0,0,1,1,1,0,2,2};
    assert(player_display_set_video_format(&f)==0);
    player_display_stats_t stats;
    player_display_get_stats(&stats);
    assert(stats.width==1280 && stats.height==720 && stats.format.coded_width==640);
    assert(stats.viewport_x==160 && stats.viewport_y==0 && stats.viewport_width==960 && stats.viewport_height==720);
    player_display_frame_t a=frame(0);
    a.capacity=640u*480u*3u/2u;
    assert(player_display_submit_frame(&a)==0);
    const uint32_t *desc=last_lcd->CompData.LayerDesc[last_lcd->CompData.comp_next_frame][0].words;
    const uint32_t *frame_desc=last_lcd->CompData.FrameDesc[last_lcd->CompData.comp_next_frame].words;
    assert(desc[0]==(640u|(480u<<16)) && desc[3]==(960u|(720u<<16)));
    assert(desc[6]==160 && desc[7]==341 && desc[8]==341 && (frame_desc[9]&1u));
    assert(submitted_layers[0].disp_pos_x>2047u); /* Real HAL's native-panel clamp underflows. */
    assert(desc[6]==160); /* The actual descriptor is restored to logical-canvas coordinates. */
    assert(desc[9]==640 && desc[10]==CPHYSADDR(a.nv12)+640u*480u && desc[11]==640);
    assert(fake_dpu.CSC_MULT_YRV==0x072c04a8 && fake_dpu.CSC_MULT_GUGV==0x022200da);
    assert(fake_dpu.CSC_MULT_BU==0x873 && fake_dpu.CSC_SUB_YUV==0x00800010);
    f.visible_height=478;
    assert(player_display_set_video_format(&f)==-EBUSY); /* Never reinterpret an active NV12 hold. */
    loaded_descriptor=next_descriptor; loaded_y=desc[2]; loaded_ui=0;
    fake_dpu.LAY0_SITE=loaded_y+64u; fake_dpu.LAY1_SITE=0;
    fake_dpu.WDMA_SITE=frame_desc[3]+64u;
    composer_started=false;
    fake_dpu.DC_INT_FLAG=DC_INTC_INT_EOW_0; lcd_irq(IRQ_INTC0_LCD,NULL);
    rotation_event(PLAYER_ROTATOR_SOF|PLAYER_ROTATOR_WORKING);
    clock_us+=33033; interrupt_only();
    player_display_completion_t c;
    assert(player_display_poll_completion(&c)==1 && c.has_latched);
    assert(player_display_stop()==0);

    /* A cropped portrait source keeps native GUI and consumes coded rows. */
    memset(cached_writebacks,0,sizeof(cached_writebacks));
    assert(player_display_init()==0 && player_display_start()==0);
    f=(player_video_format_t){496,656,480,640,8,2,1,1,6,1,2,2};
    assert(player_display_set_video_format(&f)==0 && !rot_irq);
    a=frame(0); a.capacity=512u*656u*3u/2u;
    assert(player_display_submit_frame(&a)==0);
    desc=last_lcd->CompData.LayerDesc[last_lcd->CompData.comp_next_frame][0].words;
    assert(desc[2]==CPHYSADDR(a.nv12)+2u*512u+8u);
    assert(desc[10]==CPHYSADDR(a.nv12)+656u*512u+512u+8u);
    assert(desc[0]==(480u|(640u<<16)) && desc[3]==(720u|(960u<<16)));
    assert(desc[6]==(160u<<16));
    latch();
    assert(player_display_poll_completion(&c)==1 && c.has_latched);
    assert(player_display_stop()==0);
}

static void native_chain_insertion(void)
{
    assert(player_display_init()==0 && player_display_start()==0);
    unsigned first_start=starts, first_post=descriptor_posts, first_channel=channel_writes;
    player_display_frame_t a=frame(0), b=frame(1);
    player_display_completion_t completion;
    uint32_t serial;
    assert(player_display_submit_frame(&a)==0);
    assert(starts==first_start+1u && descriptor_posts==first_post+1u);
    assert(channel_writes==first_channel+1u && composer_started);
    latch();
    assert(player_display_poll_completion(&completion)==1 && completion.latched.slot==0);
    for (unsigned i=0;i<8;++i) {
        const uint32_t *ui=(const uint32_t *)(uintptr_t)(i&1u ? 0x82400000u : 0x82000000u);
        unsigned old_index=last_lcd->CompData.comp_next_frame;
        assert(player_display_submit_overlay(ui,PLAYER_DISPLAY_OVERLAY_BYTES,100u+i)==0);
        assert(last_lcd->CompData.comp_next_frame==(old_index+1u)%3u);
        assert(starts==first_start+1u && channel_writes==first_channel+1u);
        assert(descriptor_posts==first_post+2u+i && composer_started);
        interrupt_only(); /* Posting a next address cannot acknowledge old scanout. */
        assert(player_display_poll_overlay_completion(&serial)==0);
        latch();
        assert(player_display_poll_overlay_completion(&serial)==1 && serial==100u+i);
        assert(player_display_poll_completion(&completion)==0);
    }
    assert(player_display_submit_frame(&b)==0);
    assert(starts==first_start+1u && channel_writes==first_channel+1u);
    assert(descriptor_posts==first_post+10u);
    latch();
    assert(player_display_poll_completion(&completion)==1 && completion.latched.slot==1 &&
           completion.has_retired && completion.retired.slot==0);
    assert(player_display_stop()==0 && !composer_started);

    assert(player_display_init()==0 && player_display_start()==0);
    assert(player_display_submit_frame(&a)==0);
    assert(starts==first_start+2u && channel_writes==first_channel+2u);
    assert(descriptor_posts==first_post+11u && composer_started);
    latch();
    assert(player_display_poll_completion(&completion)==1 && completion.latched.slot==0);
    assert(player_display_stop()==0);
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
    assert(player_display_submit_frame(&b) == 0 && descriptor_posts == 2 && starts == 1);
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
    unsigned before = descriptor_posts;
    latch();
    assert(player_display_poll_overlay_completion(&serial) == 0 && descriptor_posts == before + 1);
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
    scaled_format_route();
    native_chain_insertion();
    puts("PASS production display: legacy guards/priority, CUSTOM EOW/SOF/EOD ownership, paused GUI-only/repeat, GUI route ACK, native address-only insertion/restart, wide one-shot START, stop pin");
    return 0;
}
