/* Touch and three-key local player UI. All LVGL calls stay on its owner task. */
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
static lv_obj_t *row_names[10], *row_types[10], *volume_label, *pause_label, *audio_art;
static lv_indev_drv_t pointer_driver;
static lv_indev_t *pointer;
static lv_point_t touch_point;
static bool touch_pressed, touch_suppressed, touch_activity;
static void *allocations[2];
static lv_color_t *draw_pixels;
static player_gui_stats_t stats;
static enum player_gui_page return_page;
static uint32_t last_ms, next_refresh, revision, media_session, request_id;
static bool initialized, command_deferred;
static unsigned diagnostic_offset;
static uint32_t library_generation, library_offset, volume_request;
static unsigned requested_volume;
static bool volume_pending;
static uint32_t controls_activity_ms;
static bool key_activity;

enum touch_action {
    TOUCH_MENU = 1, TOUCH_PAUSE, TOUCH_VOLUME_DOWN, TOUCH_VOLUME_UP,
    TOUCH_LIBRARY, TOUCH_UP, TOUCH_PREVIOUS, TOUCH_NEXT, TOUCH_REFRESH,
    TOUCH_DIAGNOSTICS, TOUCH_BACK, TOUCH_MENU_ITEM = 32, TOUCH_ROW = 64
};
static void touch_event(lv_event_t *event);

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
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        const char *caption, enum touch_action id, bool primary)
{
    lv_obj_t *object = box(parent, x, y, width, height, primary ? 0x65dfc4 : 0x1b2b38, 255);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(primary ? 0x91f2db : 0x2b4556), LV_STATE_PRESSED);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(object, touch_event, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
    lv_obj_t *caption_label = label(object, 0, 0, width, caption, primary ? 0x0b2421 : 0xdbe8ee);
    lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(caption_label, LV_ALIGN_CENTER, 0, 0);
    return caption_label;
}

static void context_changed(void)
{
    /* Cancel LVGL's previous pressed object independently of the sampled
     * physical release. Merely feeding RELEASED would emit CLICKED, and the
     * release may already have arrived while the output route was paused. */
    if (pointer) lv_indev_wait_release(pointer);
    if (touch_pressed) touch_suppressed = true;
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
    memset(row_names, 0, sizeof(row_names));
    memset(row_types, 0, sizeof(row_types));
    progress_track = progress_fill = NULL;
    playback_controls = NULL;
    stats.controls_visible = page == PLAYER_GUI_PLAYBACK;
    controls_activity_ms = last_ms;
    lv_obj_set_style_bg_opa(screen, page == PLAYER_GUI_PLAYBACK ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    audio_art = volume_label = pause_label = NULL;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0b121b), 0);
    top_bar = box(screen, 24, 20, 1232, 98, 0x111e2a, page == PLAYER_GUI_PLAYBACK ? 220 : 255);
    lv_obj_set_style_radius(top_bar, 16, 0);
    heading = label(top_bar, 24, 12, 1040, page == PLAYER_GUI_LIBRARY ? "媒体库 / LIBRARY" :
                    page == PLAYER_GUI_PLAYBACK ? "正在播放" : "运行诊断 / STATUS", 0x65dfc4);
    path_label = label(top_bar, 24, 55, 1040, "", 0xdce6ef);
    button(top_bar, 1088, 20, 120, 56, page == PLAYER_GUI_DIAGNOSTICS ? "返回" : "菜单",
           page == PLAYER_GUI_DIAGNOSTICS ? TOUCH_BACK : TOUCH_MENU, false);
    bottom_bar = box(screen, 24, 676, 1232, 40, 0x0b121b, page == PLAYER_GUI_PLAYBACK ? 190 : 255);
    footer = label(bottom_bar, 8, 4, 1216, "", 0x8196a8);
    notice = label(screen, 40, 626, 1200, "", 0xf1c86b);
    if (page == PLAYER_GUI_LIBRARY) {
        lv_obj_t *rail = box(screen, 24, 134, 212, 478, 0x111e2a, 255);
        lv_obj_set_style_radius(rail, 16, 0);
        button(rail, 16, 16, 180, 62, "返回上级", TOUCH_UP, false);
        button(rail, 16, 94, 180, 62, "上一页", TOUCH_PREVIOUS, false);
        button(rail, 16, 172, 180, 62, "下一页", TOUCH_NEXT, false);
        button(rail, 16, 250, 180, 62, "刷新目录", TOUCH_REFRESH, false);
        button(rail, 16, 328, 180, 62, "运行诊断", TOUCH_DIAGNOSTICS, false);
        label(rail, 22, 420, 180, "本地 · TF", 0x8196a8);
        for (unsigned i = 0; i < 10; ++i) {
            rows[i] = box(screen, 252, 134 + (int)i * 48, 1004, 44, 0x12212d, 255);
            lv_obj_set_style_radius(rows[i], 8, 0);
            lv_obj_add_flag(rows[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(rows[i], touch_event, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)(TOUCH_ROW + i));
            row_types[i] = label(rows[i], 14, 6, 80, "", 0x65dfc4);
            row_names[i] = label(rows[i], 104, 6, 872, "", 0xe8eef2);
        }
    } else if (page == PLAYER_GUI_PLAYBACK) {
        playback_controls = box(screen, 116, 500, 1048, 170, 0x111e2a, 232);
        lv_obj_set_style_radius(playback_controls, 18, 0);
        progress_track = box(playback_controls, 24, 22, 1000, 6, 0x394b59, 255);
        progress_fill = box(progress_track, 0, 0, 1, 6, 0x65dfc4, 255);
        rows[0] = label(playback_controls, 24, 45, 680, "", 0xf0f6fa);
        volume_label = label(playback_controls, 728, 45, 296, "", 0x9db0c1);
        lv_obj_set_style_text_align(volume_label, LV_TEXT_ALIGN_RIGHT, 0);
        pause_label = button(playback_controls, 24, 100, 208, 52, "暂停", TOUCH_PAUSE, true);
        button(playback_controls, 248, 100, 96, 52, "－", TOUCH_VOLUME_DOWN, false);
        button(playback_controls, 360, 100, 96, 52, "＋", TOUCH_VOLUME_UP, false);
        button(playback_controls, 472, 100, 240, 52, "返回媒体库", TOUCH_LIBRARY, false);
        button(playback_controls, 728, 100, 296, 52, "快捷菜单", TOUCH_MENU, false);
        audio_art = box(screen, 464, 200, 352, 88, 0x142d38, 255);
        lv_obj_set_style_radius(audio_art, 20, 0);
        label(audio_art, 48, 28, 280, "♪  本地音频", 0x65dfc4);
        rows[1] = label(screen, 48, 310, 1184, "", 0xf1c86b);
        lv_obj_set_style_text_align(rows[1], LV_TEXT_ALIGN_CENTER, 0);
    } else {
        for (unsigned i = 0; i < 10; ++i) {
            lv_obj_t *card = box(screen, 24, 134 + (int)i * 48, 1232, 44, i % 2 ? 0x101b26 : 0x142330, 255);
            lv_obj_set_style_radius(card, 8, 0);
            rows[i] = label(card, 20, 6, 1192, "", 0xdce6ef);
        }
        button(screen, 40, 620, 180, 48, "向上", TOUCH_PREVIOUS, false);
        button(screen, 236, 620, 180, 48, "向下", TOUCH_NEXT, false);
    }
    revision = UINT32_MAX;
}

static void show_menu(void)
{
    if (stats.menu_open) { close_menu(); return; }
    context_changed();
    stats.menu_open = 1;
    stats.menu_selected = 0;
    menu = box(screen, 0, 0, 1280, 720, 0x050a10, 160);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *card = box(menu, 340, 150, 600, 420, 0x172735, 255);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(card, 20, 0);
    label(card, 28, 24, 544, "快捷菜单", 0x65dfc4);
    const char *items[] = {view.state != PLAYER_SESSION_IDLE ? "停止并返回媒体库" : "刷新媒体库",
                           stats.page == PLAYER_GUI_DIAGNOSTICS ? "返回上页" : "查看诊断", "关闭菜单"};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *caption = button(card, 24, 90 + (int)i * 82, 552, 64, items[i],
                                  (enum touch_action)(TOUCH_MENU_ITEM + i), false);
        menu_rows[i] = lv_obj_get_parent(caption);
    }
    label(card, 28, 364, 544, "A/B 选择 · Start 确认", 0x8196a8);
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
    else if (kind == PLAYER_SESSION_PLAY_PATH)
        snprintf(deferred.path, sizeof(deferred.path), "%s", view.media_path);
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
        if (event == PLAYER_INPUT_START_SHORT)
            (void)send_command(view.state == PLAYER_SESSION_ENDED ? PLAYER_SESSION_PLAY_PATH :
                               PLAYER_SESSION_TOGGLE_PAUSE, 0);
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

static void touch_event(lv_event_t *event)
{
    if (command_deferred || touch_suppressed) return;
    /* LVGL retains the pressed object while dragging. Releasing outside its
     * hit rectangle cancels the tap instead of opening the original file. */
    lv_indev_t *active = lv_indev_get_act();
    if (active) {
        lv_point_t point;
        lv_area_t area;
        lv_indev_get_point(active, &point);
        lv_obj_get_coords(lv_event_get_target(event), &area);
        if (point.x < area.x1 || point.x > area.x2 || point.y < area.y1 || point.y > area.y2) return;
    }
    unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (id >= TOUCH_ROW && id < TOUCH_ROW + 10u) {
        unsigned index = id - TOUCH_ROW;
        if (stats.page == PLAYER_GUI_LIBRARY && index < view.library_count)
            (void)send_command(PLAYER_SESSION_LIBRARY_ACTIVATE, (int32_t)view.entries[index].index);
        return;
    }
    if (id >= TOUCH_MENU_ITEM && id < TOUCH_MENU_ITEM + 3u) {
        stats.menu_selected = id - TOUCH_MENU_ITEM;
        action(PLAYER_INPUT_START_SHORT);
        return;
    }
    switch (id) {
    case TOUCH_MENU: show_menu(); break;
    case TOUCH_PAUSE: action(PLAYER_INPUT_START_SHORT); break;
    case TOUCH_VOLUME_DOWN: action(PLAYER_INPUT_A); break;
    case TOUCH_VOLUME_UP: action(PLAYER_INPUT_B); break;
    case TOUCH_UP: (void)send_command(PLAYER_SESSION_LIBRARY_UP, 0); break;
    case TOUCH_PREVIOUS:
    case TOUCH_NEXT:
        if (stats.page == PLAYER_GUI_DIAGNOSTICS)
            action(id == TOUCH_PREVIOUS ? PLAYER_INPUT_A : PLAYER_INPUT_B);
        else (void)send_command(PLAYER_SESSION_LIBRARY_MOVE, id == TOUCH_PREVIOUS ? -10 : 10);
        break;
    case TOUCH_REFRESH: (void)send_command(PLAYER_SESSION_LIBRARY_OPEN, 0); break;
    case TOUCH_LIBRARY:
        if (send_command(PLAYER_SESSION_STOP, 0)) build_page(PLAYER_GUI_LIBRARY);
        break;
    case TOUCH_DIAGNOSTICS:
        return_page = stats.page;
        build_page(PLAYER_GUI_DIAGNOSTICS);
        break;
    case TOUCH_BACK: build_page(return_page); break;
    default: break;
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
                const char *extension = strrchr(view.entries[i].label, '.');
                text(row_names[i], view.entries[i].label);
                snprintf(line, sizeof(line), "%.4s", view.entries[i].is_directory ? "DIR" :
                         extension && extension[1] ? extension + 1 : "FILE");
                for (char *p = line; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
                text(row_types[i], line);
            } else { text(row_names[i], ""); text(row_types[i], ""); }
            uint32_t color = selected ? 0x244b50 : i % 2u ? 0x101b26 : 0x142330;
            if (lv_color_to32(lv_obj_get_style_bg_color(rows[i], 0)) != lv_color_to32(lv_color_hex(color))) {
                lv_obj_set_style_bg_color(rows[i], lv_color_hex(color), 0);
            }
            if (i < view.library_count) lv_obj_clear_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        snprintf(line, sizeof(line), "点按文件打开 · A/B 选择 · Start 确认     %lu / %s%lu",
                 (unsigned long)(view.library_count ? view.library_selected + 1u : 0u),
                 view.library_total_known ? "" : ">=", (unsigned long)view.library_total);
        text(footer, line);
        text(notice, view.library_error ? "读取目录失败，可在菜单刷新媒体库重试" :
             view.error_code ? view.error :
             view.library_scanning ? "正在扫描目录，按键可继续浏览" :
             !view.library_count ? "此目录暂无支持的音视频文件" : "");
    } else if (stats.page == PLAYER_GUI_PLAYBACK) {
        text(heading, state_name());
        text(path_label, basename_utf8(view.media_path));
        uint32_t elapsed = view.elapsed_samples / 48000u;
        uint64_t total = view.duration_us / 1000000u;
        char duration[24];
        if (view.duration_us) snprintf(duration,sizeof(duration),"%02lu:%02lu",
            (unsigned long)(total/60u),(unsigned long)(total%60u));
        else snprintf(duration,sizeof(duration),"--:--");
        snprintf(line, sizeof(line), "%s   %02lu:%02lu / %s",
                 state_name(), (unsigned long)(elapsed / 60u), (unsigned long)(elapsed % 60u),
                 duration);
        text(rows[0], line);
        snprintf(line, sizeof(line), "音量 %lu%%", (unsigned long)view.volume_percent);
        text(volume_label, line);
        text(pause_label, view.state == PLAYER_SESSION_PAUSED ? "继续播放" :
                          view.state == PLAYER_SESSION_ENDED ? "重新播放" : "暂停");
        uint64_t elapsed_us = (uint64_t)view.elapsed_samples * 1000000u / 48000u;
        unsigned width = !view.duration_us ? 1u :
            view.state == PLAYER_SESSION_ENDED || elapsed_us >= view.duration_us ? 1000u :
            (unsigned)(elapsed_us * 1000u / view.duration_us);
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
        text(rows[1], view.error_code ? view.error : view.has_video ? "" : media_label);
        if (view.has_video) lv_obj_add_flag(audio_art, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(audio_art, LV_OBJ_FLAG_HIDDEN);
        lv_opa_t background = view.has_video ? LV_OPA_TRANSP : LV_OPA_COVER;
        if (lv_obj_get_style_bg_opa(screen, 0) != background)
            lv_obj_set_style_bg_opa(screen, background, 0);
        text(footer, "A/B 音量 · Start 暂停/继续 · 长按 Start 菜单");
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

static void read_touch(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point = touch_point;
    data->state = touch_pressed && !touch_suppressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
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
    touch_point.x = touch_point.y = 0;
    touch_pressed = touch_suppressed = touch_activity = false;
    lv_indev_drv_init(&pointer_driver);
    pointer_driver.type = LV_INDEV_TYPE_POINTER;
    pointer_driver.disp = display;
    pointer_driver.read_cb = read_touch;
    pointer = lv_indev_drv_register(&pointer_driver);
    if (!pointer) { (void)player_gui_deinit(1); return -ENOMEM; }
    /* Process pointer callbacks before the surface ownership gate below.
     * Running them inside lv_timer_handler could invalidate/resume refresh
     * after that gate while a DMA surface is still pinned. */
    lv_timer_pause(pointer_driver.read_timer);
    screen = lv_scr_act();
    lv_obj_remove_style_all(screen);
    initialized = true;
    if (output.attach) output.attach(output.context, true);
    stats.generation = 1;
    request_id = media_session = last_ms = next_refresh = 0;
    library_generation = library_offset = volume_request = requested_volume = 0;
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

void player_gui_sample_touch(uint32_t now_ms, bool pressed, unsigned x, unsigned y)
{
    (void)now_ms;
    if (!initialized) return;
    if (x >= PLAYER_GUI_WIDTH || y >= PLAYER_GUI_HEIGHT) pressed = false;
    if (pressed) {
        touch_activity = true;
        if (!touch_pressed && ((!stats.controls_visible && stats.page == PLAYER_GUI_PLAYBACK) ||
                               command_deferred)) touch_suppressed = true;
        touch_point.x = (lv_coord_t)x;
        touch_point.y = (lv_coord_t)y;
    } else touch_suppressed = false;
    touch_pressed = pressed;
}

static void update_controls(uint32_t now_ms)
{
    INPUT_LOCK();
    bool active = key_activity || touch_activity || touch_pressed;
    key_activity = false;
    touch_activity = false;
    INPUT_UNLOCK();
    if (stats.page != PLAYER_GUI_PLAYBACK) return;
    if (active || !view.has_video || view.state != PLAYER_SESSION_PLAYING ||
        stats.menu_open || view.error_code || view.resources_pinned)
        controls_activity_ms = now_ms;
    bool visible = now_ms - controls_activity_ms < 3000u;
    if (visible == (stats.controls_visible != 0)) return;
    stats.controls_visible = visible;
    lv_obj_t *objects[] = {top_bar, bottom_bar, playback_controls, rows[1], audio_art};
    for (unsigned i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i) {
        if (visible && (objects[i] != audio_art || !view.has_video)) lv_obj_clear_flag(objects[i], LV_OBJ_FLAG_HIDDEN);
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
            if (library_generation && (library_generation != view.library_generation ||
                                      library_offset != view.library_offset) &&
                stats.page == PLAYER_GUI_LIBRARY) context_changed();
            library_generation = view.library_generation;
            library_offset = view.library_offset;
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
    lv_indev_read_timer_cb(pointer_driver.read_timer);
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
    if (pointer) lv_indev_delete(pointer);
    pointer = NULL;
    if (display) lv_disp_remove(display);
    display = NULL;
    screen = menu = NULL;
    free(draw_pixels);
    draw_pixels = NULL;
    for (unsigned i = 0; i < 2; ++i) { free(allocations[i]); allocations[i] = NULL; }
    initialized = false;
    return 0;
}
