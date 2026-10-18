#include "handheld_ui.h"
#include "resources.h"
#include "storage.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { KEY_A=1, KEY_B=2, KEY_SELECT=4, KEY_START=8, KEY_RIGHT=0x10, KEY_LEFT=0x20,
       KEY_UP=0x40, KEY_DOWN=0x80, KEY_R=0x100, KEY_L=0x200 };
static ui_view_t view;
static storage_entry_t entries[UI_VIEW_ROWS];
static char directory[513], launch_path[513], game_path[513];
static uint32_t selection, total;
static size_t count;
static uint16_t last_keys, repeat_key;
static uint32_t repeat_at;
static int last_menu, release_gate, dirty, legacy_root, sound_on;
static enum ui_screen notice_parent, resources_parent;
static enum ui_action notice_retry;
static unsigned volume_percent, frameskip, speed_multiplier, settings_selected;

static void copy_text(char *out, size_t size, const char *text)
{
    if (out == text || !size) return;
    if (!text) text = "";
    size_t length = strlen(text);
    if (length >= size) {
        length = size - 1u;
        /* Keep UI-only shortening on a UTF-8 boundary. Path buffers use
         * explicit capacity errors and never use this shortened text. */
        while (length && ((unsigned char)text[length] & 0xc0u) == 0x80u)
            --length;
    }
    memcpy(out, text, length);
    out[length] = 0;
}

static const char *base_name(const char *path)
{ const char *name = strrchr(path, '/'); return name ? name + 1 : path; }

static const char *root_path(void)
{ return legacy_root ? REGBA_LEGACY_ROM_DIR : REGBA_ROM_DIR; }

static void latch_input(void)
{ release_gate = 1; repeat_key = 0; dirty = 1; }

static void list_page(void)
{
    view.screen = UI_LIBRARY;
    copy_text(view.title, sizeof(view.title), "游戏列表");
    copy_text(view.subtitle, sizeof(view.subtitle), directory);
    copy_text(view.footer, sizeof(view.footer), "A 启动  B 返回  L/R 翻页\nSELECT 切换目录  START 资源路径");
    view.message[0] = 0;
    view.error = 0;
    uint32_t first = selection / UI_VIEW_ROWS * UI_VIEW_ROWS;
    int result = storage_scan_gba(directory, first, entries, UI_VIEW_ROWS, &total, &count);
    if (!result && total && selection >= total) {
        selection = total - 1;
        first = selection / UI_VIEW_ROWS * UI_VIEW_ROWS;
        result = storage_scan_gba(directory, first, entries, UI_VIEW_ROWS, &total, &count);
    }
    if (result < 0) {
        count = total = selection = first = 0;
        view.error = 1;
        copy_text(view.message, sizeof(view.message), result == -ENAMETOOLONG ?
                  "文件名或路径过长。请缩短名称后按 A 重试。" : result == -ENOENT ?
                  "目录不存在。请将 .gba 游戏放入此目录，或按 SELECT 切换目录。" :
                  "无法读取 TF 卡。请检查卡和接线，按 A 重试。");
    } else if (!total) {
        copy_text(view.message, sizeof(view.message),
                  "此目录没有 GBA 游戏。请放入 .gba 文件，或按 SELECT 切换目录。");
    }
    view.item_count = (unsigned)count;
    view.total = total;
    view.first = first;
    view.selected = selection - first;
    for (size_t i = 0; i < count; ++i) {
        copy_text(view.items[i], sizeof(view.items[i]), entries[i].name);
        view.directories[i] = entries[i].directory;
        if (entries[i].directory)
            copy_text(view.details[i], sizeof(view.details[i]), "文件夹");
        else
            snprintf(view.details[i], sizeof(view.details[i]), "%lu.%01lu MB  /  GBA",
                     (unsigned long)(entries[i].size / 1048576u),
                     (unsigned long)((entries[i].size % 1048576u) * 10u / 1048576u));
    }
    dirty = 1;
}

void handheld_ui_library(int loaded)
{
    view.game_loaded = loaded;
    list_page();
    latch_input();
}

void handheld_ui_pause(const char *path)
{
    static const char *items[] = {
        "继续游戏", "保存进度", "读取存档", "游戏设置", "切换游戏", "资源路径", "退出游戏"
    };
    copy_text(game_path, sizeof(game_path), path);
    view.screen = UI_PAUSE;
    copy_text(view.title, sizeof(view.title), "游戏菜单");
    copy_text(view.subtitle, sizeof(view.subtitle), base_name(game_path));
    copy_text(view.footer, sizeof(view.footer), "A 确认  B/Y 继续游戏\n退出或切换前会完成存档保存");
    view.item_count = view.total = 7;
    view.first = view.selected = 0;
    view.game_loaded = 1;
    view.error = 0;
    view.message[0] = 0;
    for (unsigned i = 0; i < view.item_count; ++i) {
        copy_text(view.items[i], sizeof(view.items[i]), items[i]);
        view.details[i][0] = 0;
        view.directories[i] = 0;
    }
    latch_input();
}

static void settings_details(void)
{
    snprintf(view.details[0], sizeof(view.details[0]), "%u%%", volume_percent);
    if (frameskip)
        snprintf(view.details[1], sizeof(view.details[1]), "跳过 %u 帧", frameskip);
    else copy_text(view.details[1], sizeof(view.details[1]), "关闭");
    snprintf(view.details[2], sizeof(view.details[2]), "%u 倍", speed_multiplier);
}

static void settings_page(void)
{
    static const char *items[] = { "音量", "跳帧", "运行速度" };
    view.screen = UI_SETTINGS;
    copy_text(view.title, sizeof(view.title), "游戏设置");
    copy_text(view.subtitle, sizeof(view.subtitle), base_name(game_path));
    copy_text(view.footer, sizeof(view.footer), "左/右 调整  A 下一档  B/Y 返回\n加速时静音，倍速为目标速度");
    view.item_count = view.total = 3;
    view.first = 0;
    view.selected = settings_selected;
    view.error = 0;
    view.message[0] = 0;
    for (unsigned i = 0; i < view.item_count; ++i) {
        copy_text(view.items[i], sizeof(view.items[i]), items[i]);
        view.directories[i] = 0;
    }
    settings_details();
    latch_input();
}

void handheld_ui_options(unsigned volume, unsigned skip, unsigned speed)
{
    if (volume_percent == volume && frameskip == skip && speed_multiplier == speed)
        return;
    volume_percent = volume;
    frameskip = skip;
    speed_multiplier = speed;
    if (view.screen == UI_SETTINGS) { settings_details(); dirty = 1; }
}

static void resources_page(void)
{
    resources_parent = view.screen;
    view.screen = UI_RESOURCES;
    copy_text(view.title, sizeof(view.title), "资源路径");
    copy_text(view.subtitle, sizeof(view.subtitle), "TF / FAT32");
#ifdef EMU_ENGINE_MGBA
    const char *names[] = { "游戏目录", "旧版游戏目录", "游戏存档" };
    const char *paths[] = {REGBA_ROM_DIR, REGBA_LEGACY_ROM_DIR, MGBA_SAVE_DIR};
    view.item_count = view.total = 3;
#else
    const char *names[] = { "游戏目录", "旧版游戏目录", "游戏存档", "可选 BIOS", "游戏配置" };
    const char *paths[] = {REGBA_ROM_DIR, REGBA_LEGACY_ROM_DIR, REGBA_SAVE_DIR, REGBA_BIOS_FILE, REGBA_GAME_CONFIG_FILE};
    view.item_count = view.total = 5;
#endif
    view.first = view.selected = 0;
    view.error = 0;
    view.message[0] = 0;
    for (unsigned i = 0; i < view.item_count; ++i) {
        copy_text(view.items[i], sizeof(view.items[i]), names[i]);
        copy_text(view.details[i], sizeof(view.details[i]), paths[i]);
        view.directories[i] = 0;
    }
    copy_text(view.footer, sizeof(view.footer), "B 返回\n存档自动识别 SRAM / EEPROM / FLASH");
    latch_input();
}

void handheld_ui_init(void)
{
    memset(&view, 0, sizeof(view));
    legacy_root = 0;
    selection = 0;
    last_keys = 0;
    last_menu = 0;
    sound_on = 1;
    volume_percent = 100;
    frameskip = settings_selected = 0;
    speed_multiplier = 1;
    copy_text(directory, sizeof(directory), REGBA_ROM_DIR);
    list_page();
    if (!total) {
        legacy_root = 1;
        copy_text(directory, sizeof(directory), root_path());
        list_page();
        if (!total) {
            legacy_root = 0;
            copy_text(directory, sizeof(directory), root_path());
            list_page();
        }
    }
    latch_input();
}

void handheld_ui_status(int connected, int loaded, int sound_enabled)
{
    if (view.connected != connected || view.game_loaded != loaded || sound_on != sound_enabled)
        dirty = 1;
    view.connected = connected;
    view.game_loaded = loaded;
    sound_on = sound_enabled;
}

void handheld_ui_notice(const char *title, const char *message, int error,
                        enum ui_action retry)
{
    if (view.screen != UI_NOTICE)
        notice_parent = view.screen;
    view.screen = UI_NOTICE;
    view.item_count = 0;
    copy_text(view.title, sizeof(view.title), title);
    copy_text(view.subtitle, sizeof(view.subtitle), "GBA");
    copy_text(view.message, sizeof(view.message), message);
    copy_text(view.footer, sizeof(view.footer), retry ? "A 重试  B 返回" : "A/B 返回");
    view.error = error;
    notice_retry = retry;
    latch_input();
}

void handheld_ui_error(const char *operation, int result, enum ui_action retry)
{
    const char *reason;
    switch (-result) {
    case ENOENT: reason = "文件或目录不存在。请检查资源路径。"; break;
    case ENODEV: case ENXIO: reason = "TF 卡未就绪。请检查卡和接线。"; break;
    case ENOMEM: reason = "内存不足，无法加载这个游戏。"; break;
    case EFBIG: reason = "ROM 大小不支持，请使用不超过 32 MB 的 .gba 文件。"; break;
    case ENODATA: reason = "游戏尚未产生可保存的进度。"; break;
    case EINVAL: case ENOTSUP: reason = "文件格式不支持或存档大小不正确。"; break;
    case ENOEXEC: reason = "文件不是可运行的 GBA ROM，请检查游戏文件。"; break;
    case ENAMETOOLONG: reason = "文件路径过长，请缩短目录或文件名。"; break;
    case EROFS: case EACCES: reason = "无法写入 TF 卡，请检查卡是否可写。"; break;
    case EBUSY: reason = "正在处理上一项操作，请稍后重试。"; break;
    default: reason = "TF 读写失败。当前未保存的进度会保留，请重试。"; break;
    }
    handheld_ui_notice(operation, reason, 1, retry);
}

static void return_page(enum ui_screen page)
{
    if (page == UI_PAUSE && view.game_loaded) handheld_ui_pause(game_path);
    else if (page == UI_SETTINGS && view.game_loaded) settings_page();
    else if (page == UI_RESOURCES) {
        enum ui_screen parent = resources_parent;
        resources_page();
        resources_parent = parent;
    }
    else list_page();
    latch_input();
}

enum ui_action handheld_ui_poll(uint16_t keys, int menu_pressed, uint32_t now_ms)
{
    uint16_t pressed = keys & ~last_keys;
    int menu_edge = menu_pressed && !last_menu;
    last_keys = keys;
    last_menu = menu_pressed;
    if (release_gate) {
        if (!keys && !menu_pressed) release_gate = 0;
        return UI_ACTION_NONE;
    }
    uint16_t direction = keys & (KEY_UP | KEY_DOWN);
    if (direction == (KEY_UP | KEY_DOWN)) direction = 0;
    if (direction && direction != repeat_key) {
        repeat_key = direction;
        repeat_at = now_ms + 350u;
    } else if (direction && (int32_t)(now_ms - repeat_at) >= 0) {
        pressed |= direction;
        repeat_at = now_ms + 100u;
    } else if (!direction) repeat_key = 0;
    if (view.screen == UI_NOTICE) {
        if ((pressed & KEY_A) && notice_retry) return notice_retry;
        if (pressed & (KEY_A | KEY_B)) return_page(notice_parent);
        return UI_ACTION_NONE;
    }
    if (view.screen == UI_RESOURCES) {
        if (pressed & KEY_B) return_page(resources_parent);
        return UI_ACTION_NONE;
    }
    if (view.screen == UI_SETTINGS) {
        if ((pressed & KEY_B) || menu_edge) { return_page(UI_PAUSE); return UI_ACTION_NONE; }
        if (pressed & KEY_UP) view.selected = (view.selected + 2u) % 3u;
        if (pressed & KEY_DOWN) view.selected = (view.selected + 1u) % 3u;
        settings_selected = view.selected;
        if (pressed & (KEY_UP | KEY_DOWN)) dirty = 1;
        int lower = !!(pressed & KEY_LEFT);
        int raise = !!(pressed & (KEY_RIGHT | KEY_A));
        if (lower == raise) return UI_ACTION_NONE;
        switch (view.selected) {
        case 0: return lower ? UI_ACTION_VOLUME_DOWN : UI_ACTION_VOLUME_UP;
        case 1: return lower ? UI_ACTION_FRAMESKIP_DOWN : UI_ACTION_FRAMESKIP_UP;
        case 2: return lower ? UI_ACTION_SPEED_DOWN : UI_ACTION_SPEED_UP;
        }
        return UI_ACTION_NONE;
    }
    if (view.screen == UI_PAUSE) {
        if ((pressed & KEY_B) || menu_edge) return UI_ACTION_RESUME;
        if (pressed & KEY_UP) view.selected = (view.selected + 6u) % 7u;
        if (pressed & KEY_DOWN) view.selected = (view.selected + 1u) % 7u;
        if (pressed & (KEY_UP | KEY_DOWN)) dirty = 1;
        if (pressed & (KEY_A | KEY_START)) {
            switch (view.selected) {
            case 0: return UI_ACTION_RESUME;
            case 1: return UI_ACTION_SAVE;
            case 2:
                handheld_ui_notice("读取存档", "将放弃当前未保存的进度，并重新启动游戏。\n启动后，请在游戏内选择继续游戏，读取已保存的进度。", 0, UI_ACTION_RELOAD);
                copy_text(view.footer, sizeof(view.footer), "A 读取并重启  B 取消");
                break;
            case 3: settings_page(); break;
            case 4: return UI_ACTION_SWITCH;
            case 5: resources_page(); break;
            case 6: return UI_ACTION_EXIT;
            }
        }
        return UI_ACTION_NONE;
    }
    if (menu_edge && view.game_loaded) {
        handheld_ui_pause(game_path);
        return UI_ACTION_NONE;
    }
    if (pressed & KEY_START) { resources_page(); return UI_ACTION_NONE; }
    if (pressed & KEY_SELECT) {
        legacy_root = !legacy_root;
        selection = 0;
        copy_text(directory, sizeof(directory), root_path());
        list_page();
        return UI_ACTION_NONE;
    }
    if (pressed & KEY_B) {
        if (strcmp(directory, root_path())) {
            char *slash = strrchr(directory, '/');
            if (slash) *slash = 0;
            selection = 0;
            list_page();
        } else if (view.game_loaded) return UI_ACTION_RESUME;
    }
    if (total) {
        uint32_t previous = selection;
        if ((pressed & KEY_UP) && selection) --selection;
        if ((pressed & KEY_DOWN) && selection + 1 < total) ++selection;
        if (pressed & KEY_L) selection = selection >= UI_VIEW_ROWS ? selection - UI_VIEW_ROWS : 0;
        if (pressed & KEY_R) selection = selection + UI_VIEW_ROWS < total ? selection + UI_VIEW_ROWS : total - 1;
        if (previous != selection) {
            if (selection / UI_VIEW_ROWS != previous / UI_VIEW_ROWS) list_page();
            else { view.selected = selection % UI_VIEW_ROWS; dirty = 1; }
        }
    }
    if (pressed & KEY_A) {
        if (!count) return UI_ACTION_REFRESH;
        storage_entry_t *entry = &entries[view.selected];
        int length = snprintf(launch_path, sizeof(launch_path), "%s/%s", directory, entry->name);
        if (length < 0 || (size_t)length >= sizeof(launch_path)) {
            handheld_ui_error("路径过长", -ENAMETOOLONG, UI_ACTION_NONE);
        } else if (entry->directory) {
            copy_text(directory, sizeof(directory), launch_path);
            selection = 0;
            list_page();
        } else return UI_ACTION_LAUNCH;
    }
    return UI_ACTION_NONE;
}

const char *handheld_ui_launch_path(void) { return launch_path; }
ui_view_t *handheld_ui_view(void) { return &view; }
int handheld_ui_dirty(void) { return dirty; }
void handheld_ui_drawn(void) { dirty = 0; }
