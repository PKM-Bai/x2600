#include "ai_gui.h"
#include "ai_history.h"
#include "player_gui_surface.h"
#include "lvgl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static player_gui_surface_t surface;
static player_gui_output_t output;
static lv_disp_drv_t driver;
static lv_disp_draw_buf_t draw;
static lv_disp_t *display;
static lv_indev_drv_t pointer_driver;
static lv_obj_t *editor, *composition, *keyboard, *status_label, *mode_label, *candidate_labels[6];
static lv_obj_t *candidate_buttons[6];
static const char *candidates[6];
static ai_asset dictionary;
static unsigned candidate_page, candidate_count;
static bool chinese = true, touch_pressed;
static lv_point_t touch_point;
static uint32_t last_ms;
static uint16_t last_keys;
static int output_error;
static ai_gui_send_fn send_draft;
static void *send_context;
static void refresh_candidates(void)
{
    candidate_count = ai_dictionary_query(&dictionary, lv_textarea_get_text(composition), candidate_page * 6, candidates, 6);
    for (unsigned i = 0; i < 6; ++i) {
        lv_label_set_text(candidate_labels[i], i < candidate_count ? candidates[i] : "");
        if (i < candidate_count) lv_obj_clear_state(candidate_buttons[i], LV_STATE_DISABLED);
        else lv_obj_add_state(candidate_buttons[i], LV_STATE_DISABLED);
    }
}
void ai_gui_set_status(const char *text)
{ if (strcmp(lv_label_get_text(status_label), text)) lv_label_set_text(status_label, text); }
const char *ai_gui_draft(void) { return lv_textarea_get_text(editor); }
static void composition_changed(lv_event_t *event)
{ (void)event; candidate_page = 0; refresh_candidates(); }
static void action(lv_event_t *event)
{
    unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (id < 6) {
        if (id < candidate_count) {
            if (strlen(ai_gui_draft()) + strlen(candidates[id]) > AI_INPUT_BYTES) {
                ai_gui_set_status("输入已达到长度上限"); return;
            }
            lv_textarea_add_text(editor, candidates[id]);
            lv_textarea_set_text(composition, "");
        }
    } else if (id == 6) {
        if (candidate_page) --candidate_page;
        refresh_candidates();
    } else if (id == 7) {
        const char *next[1];
        if (ai_dictionary_query(&dictionary, lv_textarea_get_text(composition), (candidate_page + 1) * 6, next, 1)) ++candidate_page;
        refresh_candidates();
    } else if (id == 8) {
        chinese = !chinese;
        lv_keyboard_set_textarea(keyboard, chinese ? composition : editor);
        lv_obj_set_style_border_opa(editor, chinese ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_CURSOR);
        lv_obj_set_style_border_opa(composition, chinese ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_CURSOR);
        lv_label_set_text(mode_label, chinese ? "中文 / EN" : "EN / 中文");
    } else if (id == 9) {
        if (!*ai_gui_draft()) { ai_gui_set_status("请先输入问题"); return; }
        if (strlen(ai_gui_draft()) > AI_INPUT_BYTES) { ai_gui_set_status("输入已达到长度上限"); return; }
        int result = send_draft ? send_draft(send_context, ai_gui_draft()) : -ENETDOWN;
        if (result) ai_gui_set_status("尚未连接云端，输入已保留");
        else lv_textarea_set_text(editor, "");
    } else if (id == 10) {
        lv_textarea_set_text(editor, ""); lv_textarea_set_text(composition, "");
        ai_gui_set_status("新会话：本地草稿已清空");
    }
}
static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *text)
{
    lv_obj_t *item = lv_label_create(parent);
    lv_obj_set_pos(item, (lv_coord_t)x, (lv_coord_t)y); lv_obj_set_width(item, (lv_coord_t)width);
    lv_obj_set_style_text_color(item, lv_color_hex(0xe8edf3), 0);
    lv_label_set_text(item, text); return item;
}
static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, const char *caption, unsigned id)
{
    lv_obj_t *item = lv_btn_create(parent);
    lv_obj_set_pos(item, (lv_coord_t)x, (lv_coord_t)y); lv_obj_set_size(item, (lv_coord_t)width, 46);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x24465c), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(item, action, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *caption_label = label(item, 0, 0, width - 8, caption);
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption_label);
    return caption_label;
}
static void style_textarea(lv_obj_t *item)
{
    /* No LVGL theme is enabled: text areas and cursors need explicit styles. */
    lv_obj_set_style_bg_color(item, lv_color_hex(0x152e42), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(0x35566d), 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_pad_all(item, 4, 0);
    lv_obj_set_style_pad_left(item, 8, 0);
    lv_obj_set_style_pad_right(item, 8, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(0x88e9d2), LV_PART_CURSOR);
    lv_obj_set_style_border_width(item, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_anim_time(item, 400, LV_PART_CURSOR);
    lv_obj_set_style_text_color(item, lv_color_hex(0x8ca8ba), LV_PART_TEXTAREA_PLACEHOLDER);
}
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    if (!player_gui_surface_write(&surface, area->x1, area->y1, area->x2, area->y2, (const uint32_t *)colors)) output_error = -EBUSY;
    if (lv_disp_flush_is_last(drv)) {
        (void)player_gui_surface_seal(&surface);
        lv_timer_pause(_lv_disp_get_refr_timer(display));
    }
    lv_disp_flush_ready(drv);
}
static void read_touch(lv_indev_drv_t *drv, lv_indev_data_t *data)
{ (void)drv; data->point = touch_point; data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED; }
int ai_gui_init(const player_gui_output_t *backend, const ai_asset *font, const ai_asset *dict,
                const char *status, ai_gui_send_fn send, void *context)
{
    if (!backend || !backend->submit || !backend->poll) return -EINVAL;
    output = *backend; send_draft = send; send_context = context;
    if (dict) dictionary = *dict;
    ai_gui_font_set(font);
    uint32_t *buffers[2];
    for (unsigned i = 0; i < 2; ++i) {
        void *raw = malloc(PLAYER_GUI_SURFACE_BYTES + 63);
        if (!raw) return -ENOMEM;
        buffers[i] = (uint32_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
    }
    lv_color_t *pixels = malloc(PLAYER_GUI_WIDTH * 32 * sizeof(lv_color_t));
    if (!pixels) return -ENOMEM;
    player_gui_surface_init(&surface, buffers[0], buffers[1]);
    lv_init(); lv_disp_draw_buf_init(&draw, pixels, NULL, PLAYER_GUI_WIDTH * 32);
    lv_disp_drv_init(&driver); driver.hor_res = 1280; driver.ver_res = 720;
    driver.draw_buf = &draw; driver.flush_cb = flush; driver.screen_transp = 1;
    display = lv_disp_drv_register(&driver); if (!display) return -ENOMEM;
    lv_indev_drv_init(&pointer_driver); pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.disp = display; pointer_driver.read_cb = read_touch;
    if (!lv_indev_drv_register(&pointer_driver)) return -ENOMEM;
    lv_timer_pause(pointer_driver.read_timer);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b1724), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    label(screen, 24, 16, 900, "云端 AI · 中文交流");
    button(screen, 1070, 12, 184, "新会话", 10);
    status_label = label(screen, 24, 68, 1230, status);
    label(screen, 24, 122, 1230, "重启后清空记录");
    label(screen, 24, 186, 1220, "输入问题后点击发送。按住 Start 说话，松开发送。\n语音暂未就绪。");
    editor = lv_textarea_create(screen); lv_obj_set_pos(editor, 24, 290); lv_obj_set_size(editor, 1020, 106);
    style_textarea(editor);
    lv_textarea_set_max_length(editor, AI_INPUT_BYTES);
    lv_obj_set_style_text_color(editor, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(editor, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_textarea_set_placeholder_text(editor, "请输入问题");
    button(screen, 1070, 292, 184, "发送", 9);
    mode_label = button(screen, 1070, 350, 184, "中文 / EN", 8);
    composition = lv_textarea_create(screen); lv_obj_set_pos(composition, 24, 410); lv_obj_set_size(composition, 1230, 48);
    style_textarea(composition);
    lv_textarea_set_one_line(composition, true); lv_textarea_set_max_length(composition, 64);
    lv_obj_set_height(composition, 48);
    lv_textarea_set_placeholder_text(composition, "输入全拼，例如 nihao");
    lv_textarea_set_accepted_chars(composition, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' ");
    lv_obj_set_style_text_color(composition, lv_color_hex(0x88e9d2), 0);
    lv_obj_add_event_cb(composition, composition_changed, LV_EVENT_VALUE_CHANGED, NULL);
    for (unsigned i = 0; i < 6; ++i) {
        candidate_labels[i] = button(screen, 24 + (int)i * 164, 470, 156, "", i);
        candidate_buttons[i] = lv_obj_get_parent(candidate_labels[i]);
    }
    button(screen, 1010, 470, 116, "上一页", 6); button(screen, 1138, 470, 116, "下一页", 7);
    keyboard = lv_keyboard_create(screen); lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(keyboard, 24, 530); lv_obj_set_size(keyboard, 1230, 180);
    lv_obj_set_style_pad_row(keyboard, 4, 0); lv_obj_set_style_pad_column(keyboard, 4, 0);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER); lv_keyboard_set_textarea(keyboard, composition);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x18374c), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(0xffffff), LV_PART_ITEMS);
    refresh_candidates();
    if (output.attach) output.attach(output.context, true);
    return 0;
}
int ai_gui_step(uint32_t now_ms, bool pressed, unsigned x, unsigned y, uint16_t keys)
{
    lv_tick_inc(now_ms - last_ms); last_ms = now_ms;
    touch_pressed = pressed && x < 1280 && y < 720;
    touch_point.x = (lv_coord_t)x; touch_point.y = (lv_coord_t)y;
    if ((keys & 8) && !(last_keys & 8)) ai_gui_set_status("语音尚未就绪，请先使用文字输入");
    last_keys = keys;
    if (surface.submitted) {
        uint32_t serial = 0; int result = output.poll(output.context, &serial);
        if (result < 0) return result;
        if (result) {
            if (!player_gui_surface_complete(&surface, serial)) return -EIO;
            lv_timer_resume(_lv_disp_get_refr_timer(display));
        }
    }
    lv_indev_read_timer_cb(pointer_driver.read_timer);
    if (surface.sealed) lv_timer_pause(_lv_disp_get_refr_timer(display));
    (void)lv_timer_handler();
    if (surface.sealed && !surface.submitted && !output_error) {
        int result = output.submit(output.context, surface.pixels[surface.back], PLAYER_GUI_SURFACE_BYTES,
                                   surface.serial, surface.publish_x1, surface.publish_y1, surface.publish_x2, surface.publish_y2);
        if (!result) (void)player_gui_surface_submitted(&surface);
        else if (result != -EAGAIN) return result;
    }
    return output_error;
}
