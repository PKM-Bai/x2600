/* Three-key local player UI. All LVGL calls stay on its one owner task. */
#include "player_gui.h"
#include "player_gui_surface.h"
#include "player_session.h"
#include "lvgl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef ESTALE
#define ESTALE 116
#endif
#ifdef PLAYER_GUI_FREERTOS
#include "FreeRTOS.h"
#include "task.h"
#define INPUT_LOCK() taskENTER_CRITICAL()
#define INPUT_UNLOCK() taskEXIT_CRITICAL()
#else
#define INPUT_LOCK() ((void)0)
#define INPUT_UNLOCK() ((void)0)
#endif

static player_gui_output_t output;
static player_gui_surface_t surface;
static player_input_t input;
static player_session_snapshot_t view;
static player_session_command_t deferred;
static lv_disp_drv_t driver;
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_t *display;
static lv_obj_t *screen, *heading, *path_label, *footer, *notice, *rows[10];
static lv_obj_t *top_bar, *bottom_bar, *playback_controls;
static lv_obj_t *progress_track, *progress_fill, *menu, *menu_rows[3];
static void *allocations[2];
static lv_color_t *draw_pixels;
static player_gui_stats_t stats;
static enum player_gui_page return_page;
static uint32_t last_ms, next_refresh, revision, media_session, request_id;
static bool initialized, command_deferred;
static unsigned diagnostic_offset;
static uint32_t library_generation, volume_request;
static unsigned requested_volume;
static bool volume_pending;
static uint32_t controls_activity_ms;
static bool key_activity;

static void text(lv_obj_t *object, const char *value)
{
    if (object && strcmp(lv_label_get_text(object), value)) lv_label_set_text(object, value);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height,
                     uint32_t color, unsigned opacity)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_size(object, (lv_coord_t)width, (lv_coord_t)height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, (lv_opa_t)opacity, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int width, const char *value,
                       uint32_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, (lv_coord_t)x, (lv_coord_t)y);
    lv_obj_set_width(object, (lv_coord_t)width);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(object, &player_font_32, 0);
    lv_label_set_text(object, value);
    return object;
}

static void context_changed(void)
{
    if (++stats.generation == 0) ++stats.generation;
    INPUT_LOCK();
    player_input_set_generation(&input, stats.generation);
    INPUT_UNLOCK();
}

static void close_menu(void)
{
    if (menu) lv_obj_del(menu);
    menu = NULL;
    stats.menu_open = 0;
    context_changed();
}

static void build_page(enum player_gui_page page)
{
    if (menu) { lv_obj_del(menu); menu = NULL; }
    stats.page = page;
    diagnostic_offset = 0;
    stats.menu_open = 0;
    context_changed();
    lv_obj_clean(screen);
    memset(rows, 0, sizeof(rows));
    progress_track = progress_fill = NULL;
    playback_controls = NULL;
    stats.controls_visible = page == PLAYER_GUI_PLAYBACK;
    controls_activity_ms = last_ms;
    lv_obj_set_style_bg_opa(screen, page == PLAYER_GUI_PLAYBACK ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    top_bar = box(screen, 0, 0, 1280, 108, 0x101820, page == PLAYER_GUI_PLAYBACK ? 190 : 255);
    heading = label(top_bar, 32, 14, 1120, page == PLAYER_GUI_LIBRARY ? "媒体库" :
                    page == PLAYER_GUI_PLAYBACK ? "正在播放" : "运行诊断", 0x63dfd2);
    path_label = label(top_bar, 32, 60, 1216, "", 0xd7e1e9);
    bottom_bar = box(screen, 0, 642, 1280, 78, 0x101820, page == PLAYER_GUI_PLAYBACK ? 210 : 255);
    footer = label(bottom_bar, 32, 20, 1216, "", 0xc1ccd6);
    notice = label(screen, 32, 594, 1216, "", 0xf1c86b);
    if (page == PLAYER_GUI_LIBRARY) {
        for (unsigned i = 0; i < 10; ++i) {
            rows[i] = label(screen, 48, 122 + (int)i * 46, 1184, "", 0xe8eef2);
            lv_obj_set_height(rows[i], 42);
            lv_obj_set_style_pad_left(rows[i], 12, 0);
            lv_obj_set_style_pad_top(rows[i], 3, 0);
        }
    } else if (page == PLAYER_GUI_PLAYBACK) {
        playback_controls = box(screen, 24, 500, 1232, 86, 0x101820, 205);
        rows[0] = label(playback_controls, 20, 10, 1150, "", 0xffffff);
        progress_track = box(playback_controls, 20, 57, 1192, 8, 0x4c5965, 255);
        progress_fill = box(progress_track, 0, 0, 1, 8, 0x63dfd2, 255);
        rows[1] = label(screen, 48, 310, 1184, "", 0xf1c86b);
        lv_obj_set_style_text_align(rows[1], LV_TEXT_ALIGN_CENTER, 0);
    } else {
        for (unsigned i = 0; i < 10; ++i)
            rows[i] = label(screen, 40, 126 + (int)i * 45, 1200, "", 0xe8eef2);
    }
    revision = UINT32_MAX;
}

static void show_menu(void)
{
    if (stats.menu_open) { close_menu(); return; }
    context_changed();
    stats.menu_open = 1;
    stats.menu_selected = 0;
    menu = box(screen, 704, 152, 536, 372, 0x172b38, 250);
    label(menu, 24, 20, 488, "快捷菜单", 0x63dfd2);
    const char *items[] = {view.state != PLAYER_SESSION_IDLE ? "停止并返回媒体库" : "刷新媒体库",
                           stats.page == PLAYER_GUI_DIAGNOSTICS ? "返回上页" : "查看诊断", "关闭菜单"};
    for (unsigned i = 0; i < 3; ++i) {
        menu_rows[i] = label(menu, 24, 92 + (int)i * 70, 488, items[i], 0xffffff);
        lv_obj_set_height(menu_rows[i], 52);
        lv_obj_set_style_pad_all(menu_rows[i], 8, 0);
    }
    label(menu, 24, 310, 488, "A/B 选择  Start 确认", 0xb2c2ce);
}

static bool send_command(enum player_session_command_kind kind, int32_t value)
{
    memset(&deferred, 0, sizeof(deferred));
    deferred.kind = kind;
    if (++request_id == 0) ++request_id;
    deferred.request_id = request_id;
    deferred.value = value;
    deferred.library_generation = view.library_generation;
    if (kind == PLAYER_SESSION_LIBRARY_OPEN)
        snprintf(deferred.path, sizeof(deferred.path), "%s", view.library_path);
    int result = player_session_post(&deferred);
    command_deferred = result == -EAGAIN;
    if (result && !command_deferred) text(notice, "操作失败，请查看诊断");
    return !result || command_deferred;
}

static void action(player_input_action_t event)
{
    if (event == PLAYER_INPUT_START_LONG) { show_menu(); return; }
    if (stats.menu_open) {
        if (event == PLAYER_INPUT_A) stats.menu_selected = (stats.menu_selected + 2u) % 3u;
        else if (event == PLAYER_INPUT_B) stats.menu_selected = (stats.menu_selected + 1u) % 3u;
        else if (event == PLAYER_INPUT_START_SHORT) {
            unsigned selected = stats.menu_selected;
            close_menu();
            if (selected == 0) {
                enum player_session_command_kind command = view.state != PLAYER_SESSION_IDLE
                    ? PLAYER_SESSION_STOP : PLAYER_SESSION_LIBRARY_OPEN;
                if (send_command(command, 0))
                    build_page(PLAYER_GUI_LIBRARY);
            } else if (selected == 1) {
                if (stats.page == PLAYER_GUI_DIAGNOSTICS) build_page(return_page);
                else { return_page = stats.page; build_page(PLAYER_GUI_DIAGNOSTICS); }
            }
        }
        return;
    }
    if (stats.page == PLAYER_GUI_LIBRARY) {
        if (event == PLAYER_INPUT_A || event == PLAYER_INPUT_B)
            (void)send_command(PLAYER_SESSION_LIBRARY_MOVE, event == PLAYER_INPUT_A ? -1 : 1);
        else if (event == PLAYER_INPUT_START_SHORT && view.library_count)
            (void)send_command(PLAYER_SESSION_LIBRARY_ACTIVATE, -1);
    } else if (stats.page == PLAYER_GUI_PLAYBACK) {
        if (event == PLAYER_INPUT_START_SHORT) (void)send_command(PLAYER_SESSION_TOGGLE_PAUSE, 0);
        else if (event == PLAYER_INPUT_A || event == PLAYER_INPUT_B) {
            int volume = (int)(volume_pending ? requested_volume : view.volume_percent) +
                         (event == PLAYER_INPUT_A ? -5 : 5);
            if (volume < 0) volume = 0;
            if (volume > 100) volume = 100;
            if (send_command(PLAYER_SESSION_SET_VOLUME, volume)) {
                requested_volume = (unsigned)volume;
                volume_request = request_id;
                volume_pending = true;
            }
        }
    } else {
        if (event == PLAYER_INPUT_START_SHORT) build_page(return_page);
        else if (event == PLAYER_INPUT_A && diagnostic_offset) { --diagnostic_offset; revision = UINT32_MAX; }
        else if (event == PLAYER_INPUT_B && diagnostic_offset < 3) { ++diagnostic_offset; revision = UINT32_MAX; }
    }
}

static const char *state_name(void)
{
    static const char *names[] = {"待机", "正在载入", "播放中", "已暂停", "播放结束", "播放错误"};
    return view.state <= PLAYER_SESSION_ERROR ? names[view.state] : "未知";
}

static const char *basename_utf8(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static void update_view(void)
{
    char line[1400];
    if (stats.page == PLAYER_GUI_LIBRARY) {
        text(path_label, view.library_path[0] ? view.library_path : "0:/");
        for (unsigned i = 0; i < 10; ++i) {
            bool selected = i < view.library_count && view.entries[i].index == view.library_selected;
            if (i < view.library_count) {
                snprintf(line, sizeof(line), "%s %s", view.entries[i].is_directory ? "[目录]" : "[媒体]", view.entries[i].label);
                text(rows[i], line);
            } else text(rows[i], "");
            lv_opa_t opacity = selected ? LV_OPA_COVER : LV_OPA_TRANSP;
            if (lv_obj_get_style_bg_opa(rows[i], 0) != opacity) {
                lv_obj_set_style_bg_color(rows[i], lv_color_hex(0x254756), 0);
                lv_obj_set_style_bg_opa(rows[i], opacity, 0);
            }
        }
        snprintf(line, sizeof(line), "A/B 选择  Start 打开  长按 Start 菜单     %lu / %s%lu",
                 (unsigned long)(view.library_count ? view.library_selected + 1u : 0u),
                 view.library_total_known ? "" : ">=", (unsigned long)view.library_total);
        text(footer, line);
        text(notice, view.library_error ? "读取目录失败，可在菜单刷新媒体库重试" :
             view.error_code ? view.error :
             view.library_scanning ? "正在扫描目录，按键可继续浏览" :
             !view.library_count ? "此目录暂无可播放的 AVI / WAV 文件" : "");
    } else if (stats.page == PLAYER_GUI_PLAYBACK) {
        text(heading, state_name());
        text(path_label, basename_utf8(view.media_path));
        uint32_t elapsed = view.elapsed_samples / 48000u;
        uint64_t total = view.duration_us / 1000000u;
        char duration[24];
        if (view.duration_us) snprintf(duration,sizeof(duration),"%02lu:%02lu",
            (unsigned long)(total/60u),(unsigned long)(total%60u));
        else snprintf(duration,sizeof(duration),"--:--");
        snprintf(line, sizeof(line), "%s   %02lu:%02lu / %s    音量 %lu%%",
                 state_name(), (unsigned long)(elapsed / 60u), (unsigned long)(elapsed % 60u),
                 duration, (unsigned long)view.volume_percent);
        text(rows[0], line);
        uint64_t elapsed_us = (uint64_t)view.elapsed_samples * 1000000u / 48000u;
        unsigned width = !view.duration_us ? 1u :
            view.state == PLAYER_SESSION_ENDED || elapsed_us >= view.duration_us ? 1192u :
            (unsigned)(elapsed_us * 1192u / view.duration_us);
        if (!width) width = 1;
        if (lv_obj_get_width(progress_fill) != (lv_coord_t)width) lv_obj_set_width(progress_fill, (lv_coord_t)width);
        const char *media_label=view.media_kind==1 ? "WAV音频" : view.media_kind==3 ? "MP3音频" :
                                view.media_kind==4 ? "AAC音频" : "";
        if (view.media_kind==5) {
            const char *extension=strrchr(view.media_path,'.');
            bool m4a=extension && strlen(extension)==4 &&
                (extension[1]=='m'||extension[1]=='M') && extension[2]=='4' &&
                (extension[3]=='a'||extension[3]=='A');
            media_label=m4a ? "M4A音频" : "MP4视频";
        }
        text(rows[1], view.error_code ? view.error : media_label);
        text(footer, "A/B 音量 -/+  Start 暂停/继续  长按 Start 快捷菜单");
        text(notice, view.resources_pinned ? "停止尚未完成，请重试停止" : "");
    } else {
        text(path_label, "A/B 滚动  Start 返回  长按 Start 菜单");
        char values[13][256];
        snprintf(values[0], sizeof(values[0]), "状态：%s   会话：%lu", state_name(), (unsigned long)view.session_id);
        snprintf(values[1], sizeof(values[1]), "已解码：%lu 帧   已显示：%lu 帧", (unsigned long)view.decoded_frames, (unsigned long)view.displayed_frames);
        snprintf(values[2], sizeof(values[2]), "丢帧：%lu   音频欠载：%lu   权重近似：%lu帧", (unsigned long)view.dropped_frames,
                 (unsigned long)view.audio_underruns, (unsigned long)view.weight_approximated_frames);
        snprintf(values[3], sizeof(values[3]), "音频时钟：%lu / %lu 样本", (unsigned long)view.elapsed_samples, (unsigned long)view.total_samples);
        snprintf(values[4], sizeof(values[4]), "音量：%lu%%   资源持有：%lu", (unsigned long)view.volume_percent, (unsigned long)view.resources_pinned);
        snprintf(values[5], sizeof(values[5]), "目录代次：%lu   条目数：%lu", (unsigned long)view.library_generation, (unsigned long)view.library_total);
        snprintf(values[6], sizeof(values[6]), "目录扫描：%s   错误：%ld", view.library_scanning ? "进行中" : "完成", (long)view.library_error);
        snprintf(values[7], sizeof(values[7]), "GUI 绘制：%lu   显示：%lu", (unsigned long)stats.rendered, (unsigned long)stats.presented);
        snprintf(values[8], sizeof(values[8]), "输入队列：重复丢弃 %lu / 动作丢弃 %lu", (unsigned long)input.dropped_repeats, (unsigned long)input.dropped_actions);
        snprintf(values[9], sizeof(values[9]), "最近命令：%lu   结果：%ld", (unsigned long)view.last_request_id, (long)view.last_request_result);
        snprintf(values[10], sizeof(values[10]), "播放错误：%ld   GUI 输出：%d", (long)view.error_code, stats.output_error);
        snprintf(values[11], sizeof(values[11]), "%s", view.error[0] ? view.error : "无播放错误");
        snprintf(values[12], sizeof(values[12]), "屏幕 1280x720  LVGL 8.3.10  NV12 + ARGB");
        for (unsigned i = 0; i < 10; ++i) text(rows[i], values[i + diagnostic_offset]);
        text(footer, "Start 返回上页   长按 Start 打开快捷菜单");
        text(notice, "");
    }
    revision = view.revision;
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    if (!player_gui_surface_write(&surface, area->x1, area->y1, area->x2, area->y2,
                                  (const uint32_t *)colors)) stats.output_error = -EBUSY;
    if (lv_disp_flush_is_last(drv)) {
        if (player_gui_surface_seal(&surface)) ++stats.rendered;
        lv_timer_pause(_lv_disp_get_refr_timer(display));
    }
    lv_disp_flush_ready(drv);
}

int player_gui_init(const player_gui_output_t *backend)
{
    if (initialized) return -EBUSY;
    if (!backend || !backend->submit || !backend->poll) return -EINVAL;
    memset(&stats, 0, sizeof(stats));
    memset(&view, 0, sizeof(view));
    memset(&deferred, 0, sizeof(deferred));
    command_deferred = false;
    output = *backend;
    for (unsigned i = 0; i < 2; ++i) {
        allocations[i] = malloc(PLAYER_GUI_SURFACE_BYTES + 63u);
        if (!allocations[i]) { (void)player_gui_deinit(1); return -ENOMEM; }
    }
    draw_pixels = malloc(PLAYER_GUI_WIDTH * 32u * sizeof(lv_color_t));
    if (!draw_pixels) { (void)player_gui_deinit(1); return -ENOMEM; }
    player_gui_surface_init(&surface, (uint32_t *)(((uintptr_t)allocations[0] + 63u) & ~(uintptr_t)63u),
                             (uint32_t *)(((uintptr_t)allocations[1] + 63u) & ~(uintptr_t)63u));
    player_input_init(&input);
    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, draw_pixels, NULL, PLAYER_GUI_WIDTH * 32u);
    lv_disp_drv_init(&driver);
    driver.hor_res = PLAYER_GUI_WIDTH;
    driver.ver_res = PLAYER_GUI_HEIGHT;
    driver.draw_buf = &draw_buffer;
    driver.screen_transp = 1;
    driver.flush_cb = flush;
    display = lv_disp_drv_register(&driver);
    if (!display) { (void)player_gui_deinit(1); return -ENOMEM; }
    lv_disp_set_bg_opa(display, LV_OPA_TRANSP);
    screen = lv_scr_act();
    lv_obj_remove_style_all(screen);
    initialized = true;
    if (output.attach) output.attach(output.context, true);
    stats.generation = 1;
    request_id = media_session = last_ms = next_refresh = 0;
    library_generation = volume_request = requested_volume = 0;
    volume_pending = false;
    key_activity = false;
    build_page(PLAYER_GUI_LIBRARY);
    return_page = PLAYER_GUI_LIBRARY;
    (void)send_command(PLAYER_SESSION_LIBRARY_OPEN, 0);
    return 0;
}

void player_gui_sample_keys(uint32_t now_ms, uint16_t keys)
{
    INPUT_LOCK();
    if (initialized) {
        /* Start's command waits for release/long-press recognition, but every
         * debounced press must reveal the overlay immediately. Keep only an
         * activity latch here; LVGL remains confined to the GUI task. */
        if ((keys & PLAYER_INPUT_KEY_MASK) || input.held) key_activity = true;
        player_input_sample(&input, keys, now_ms);
    }
    INPUT_UNLOCK();
}

static void update_controls(uint32_t now_ms)
{
    INPUT_LOCK();
    bool active = key_activity;
    key_activity = false;
    INPUT_UNLOCK();
    if (stats.page != PLAYER_GUI_PLAYBACK) return;
    if (active || !view.has_video || view.state != PLAYER_SESSION_PLAYING ||
        stats.menu_open || view.error_code || view.resources_pinned)
        controls_activity_ms = now_ms;
    bool visible = now_ms - controls_activity_ms < 3000u;
    if (visible == (stats.controls_visible != 0)) return;
    stats.controls_visible = visible;
    lv_obj_t *objects[] = {top_bar, bottom_bar, playback_controls, rows[1]};
    for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (visible) lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static bool pop_input(player_input_event_t *event)
{
    INPUT_LOCK();
    bool available = player_input_pop(&input, event);
    INPUT_UNLOCK();
    return available;
}

int player_gui_step(uint32_t now_ms)
{
    if (!initialized) return -ENODEV;
    lv_tick_inc(now_ms - last_ms);
    last_ms = now_ms;
    if (surface.submitted) {
        uint32_t serial = 0;
        int result = output.poll(output.context, &serial);
        if (result < 0) stats.output_error = result;
        else if (result) {
            if (!player_gui_surface_complete(&surface, serial)) stats.output_error = -ESTALE;
            else { ++stats.presented; lv_timer_resume(_lv_disp_get_refr_timer(display)); }
        }
    }
    if (output.mode) {
        uint32_t generation=0;
        bool logical=false;
        int phase=output.mode(output.context,&generation,&logical);
        if (phase<0) { stats.output_error=phase; return phase; }
        if (phase==1) {
            lv_timer_pause(_lv_disp_get_refr_timer(display));
            if (!surface.submitted && output.ack)
                (void)output.ack(output.context,generation,1);
            return stats.output_error;
        }
        if (phase==2 && stats.output_generation!=generation) {
            if (!player_gui_surface_reconfigure(&surface,logical)) return -EBUSY;
            stats.output_generation=generation;
            stats.logical_output=logical;
            lv_obj_invalidate(screen);
            lv_timer_resume(_lv_disp_get_refr_timer(display));
            if (output.ack) (void)output.ack(output.context,generation,2);
        }
    }
    if (command_deferred) {
        int result = player_session_post(&deferred);
        if (result != -EAGAIN) {
            command_deferred = false;
            if (result) text(notice, "操作失败，请查看诊断");
        }
    }
    if ((int32_t)(now_ms - next_refresh) >= 0) {
        next_refresh = now_ms + 100u;
        if (!player_session_snapshot(&view)) {
            if (library_generation && library_generation != view.library_generation &&
                stats.page == PLAYER_GUI_LIBRARY) context_changed();
            library_generation = view.library_generation;
            if (volume_pending && ((int32_t)(view.last_request_id - volume_request) >= 0 ||
                                   view.volume_percent == requested_volume)) volume_pending = false;
            if (view.session_id != media_session && view.state != PLAYER_SESSION_IDLE) {
                media_session = view.session_id;
                volume_pending = false;
                if (stats.page != PLAYER_GUI_DIAGNOSTICS) build_page(PLAYER_GUI_PLAYBACK);
            }
            if (revision != view.revision || stats.page != PLAYER_GUI_LIBRARY) update_view();
        }
    }
    player_input_event_t event;
    while (!command_deferred && pop_input(&event)) {
        if (event.generation == stats.generation) action(event.action);
    }
    if (stats.menu_open)
        for (unsigned i = 0; i < 3; ++i) {
            lv_opa_t opa = i == stats.menu_selected ? LV_OPA_COVER : LV_OPA_TRANSP;
            if (lv_obj_get_style_bg_opa(menu_rows[i], 0) != opa) {
                lv_obj_set_style_bg_color(menu_rows[i], lv_color_hex(0x35616d), 0);
                lv_obj_set_style_bg_opa(menu_rows[i], opa, 0);
            }
        }
    update_controls(now_ms);
    /* LVGL's invalidation path resumes its refresh timer automatically.
     * Page/menu changes may invalidate while our DMA surface is still pinned;
     * enforce the gate after all mutations, immediately before timers run. */
    if (surface.sealed) lv_timer_pause(_lv_disp_get_refr_timer(display));
    (void)lv_timer_handler();
    if (surface.sealed && !surface.submitted && !stats.output_error) {
        int result = output.submit(output.context, surface.pixels[surface.back],
            PLAYER_GUI_SURFACE_BYTES, surface.serial, surface.publish_x1, surface.publish_y1,
            surface.publish_x2, surface.publish_y2);
        if (!result) (void)player_gui_surface_submitted(&surface);
        else if (result != -EAGAIN) stats.output_error = result;
    }
    return stats.output_error;
}

void player_gui_get_stats(player_gui_stats_t *out)
{
    if (!out) return;
    *out = stats;
    out->dropped_repeats = input.dropped_repeats;
    out->dropped_actions = input.dropped_actions;
}

int player_gui_deinit(int quiescent)
{
    if (!quiescent) return -EBUSY;
    if (initialized && output.attach) output.attach(output.context, false);
    if (display) lv_disp_remove(display);
    display = NULL;
    screen = menu = NULL;
    free(draw_pixels);
    draw_pixels = NULL;
    for (unsigned i = 0; i < 2; ++i) { free(allocations[i]); allocations[i] = NULL; }
    initialized = false;
    return 0;
}
