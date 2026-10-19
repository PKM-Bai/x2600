#include "player_display.h"
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
struct lcd_data panel_zc502_data;
int fake_working;
static struct lay_cfg submitted_layers[4];
static uint32_t next_descriptor, loaded_descriptor, loaded_y, loaded_ui;
static unsigned word, starts, backlights;
static uint64_t clock_us;
static void (*lcd_irq)(int, void *);

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
}
uint64_t platform_time_us(void) { return clock_us; }
void vTaskDelay(unsigned ticks) { clock_us += ticks * 1000u; }
void ll_request_irq(int irq, void (*handler)(int, void *), void *argument)
{ (void)irq; (void)argument; lcd_irq = handler; }
void ll_release_irq(int irq) { (void)irq; lcd_irq = NULL; }
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
    if (lcd->CompData.comp_stopped) lcd->CompData.comp_next_frame = 0;
    else lcd->CompData.comp_next_frame = (lcd->CompData.comp_next_frame + 1) % 3u;
    memcpy(submitted_layers, layers, sizeof(submitted_layers));
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
    puts("PASS production display: descriptor guard, overlay queue/merge, continuous-video priority, paused GUI-only, separate video retirement, stop pin");
    return 0;
}
