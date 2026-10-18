/* Exercise the production handheld controller with deterministic directory
 * fixtures. No renderer, FreeRTOS, storage worker or board access is used.
 * GPL-2.0-or-later. */
#include "handheld_ui.h"
#include "resources.h"
#include "storage.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { A=0x001, B=0x002, SELECT=0x004, START=0x008,
       RIGHT=0x010, LEFT=0x020,
       UP=0x040, DOWN=0x080, R=0x100, L=0x200 };
static unsigned failures;
static uint32_t now_ms;
static int standard_result, legacy_result, child_result;
static uint32_t standard_count, legacy_count;

static const storage_entry_t standard[] = {
    {"RPG", 0, 1},
    {"Game01.gba", 1048576, 0}, {"Game02.gba", 2097152, 0},
    {"Game03.gba", 3145728, 0}, {"Game04.gba", 4194304, 0},
    {"Game05.gba", 5242880, 0}, {"Game06.gba", 6291456, 0},
    {"Game07.gba", 7340032, 0}, {"Game08.gba", 8388608, 0},
    {"Game09.gba", 9437184, 0}, {"Game10.gba", 10485760, 0},
    {"Game11.gba", 11534336, 0}, {"Game12.gba", 12582912, 0},
    {"Game13.gba", 13631488, 0}, {"Game14.gba", 14680064, 0},
    {"Game15.gba", 15728640, 0}, {"Game16.gba", 16777216, 0}
};
static const storage_entry_t legacy[] = {
    {"Legacy.gba", 1048576, 0}, {"Old", 0, 1}, {"Last.GBA", 4194304, 0}
};
static const storage_entry_t child[] = {
    {"Nested", 0, 1}, {"Child.gba", 2097152, 0}
};
static const storage_entry_t deep[] = {{"Deep.gba", 1048576, 0}};
static const storage_entry_t old_child[] = {{"Older.gba", 1048576, 0}};

typedef struct {
    char path[REGBA_RESOURCE_PATH_MAX + 1u];
    uint32_t offset;
} scan_call_t;
static scan_call_t scans[64];
static unsigned scan_count;

static void expect(int condition, const char *name)
{
    if (!condition) {
        printf("FAIL %s\n", name);
        ++failures;
    }
}

static void expect_action(const char *name, enum ui_action actual,
                          enum ui_action expected)
{
    if (actual != expected) {
        printf("FAIL %s action=%d expected=%d\n", name, (int)actual, (int)expected);
        ++failures;
    }
}

/* This stub implements the public scan contract, including error outputs
 * and directory order. It does not share controller pagination logic. */
int storage_scan_gba(const char *path, uint32_t offset,
                     storage_entry_t *out, size_t capacity,
                     uint32_t *total, size_t *count)
{
    const storage_entry_t *fixture = NULL;
    uint32_t available = 0;
    int result = 0;
    expect(total && count && (out || !capacity), "valid storage scan buffers");
    if (!total || !count || (!out && capacity)) return -EINVAL;
    *total = 0;
    *count = 0;
    if (scan_count < sizeof(scans) / sizeof(scans[0])) {
        snprintf(scans[scan_count].path, sizeof(scans[scan_count].path), "%s", path);
        scans[scan_count].offset = offset;
    } else expect(0, "scan log capacity");
    ++scan_count;
    if (!strcmp(path, REGBA_ROM_DIR)) {
        fixture = standard; available = standard_count; result = standard_result;
    } else if (!strcmp(path, REGBA_LEGACY_ROM_DIR)) {
        fixture = legacy; available = legacy_count; result = legacy_result;
    } else if (!strcmp(path, REGBA_ROM_DIR "/RPG")) {
        fixture = child; available = 2; result = child_result;
    } else if (!strcmp(path, REGBA_ROM_DIR "/RPG/Nested")) {
        fixture = deep; available = 1;
    } else if (!strcmp(path, REGBA_LEGACY_ROM_DIR "/Old")) {
        fixture = old_child; available = 1;
    } else result = -ENOENT;
    if (result < 0) return result;
    *total = available;
    for (uint32_t i = offset; i < available && *count < capacity; ++i)
        out[(*count)++] = fixture[i];
    return 0;
}

static enum ui_action poll_at(uint16_t keys, int menu, uint32_t time)
{
    now_ms = time;
    return handheld_ui_poll(keys, menu, time);
}

static void release(void)
{
    expect_action("release produces no action", poll_at(0, 0, now_ms + 10u), UI_ACTION_NONE);
}

static enum ui_action tap(uint16_t keys)
{
    enum ui_action action = poll_at(keys, 0, now_ms + 10u);
    release();
    return action;
}

static void key(uint16_t keys)
{ expect_action("navigation produces no core action", tap(keys), UI_ACTION_NONE); }

static void fixtures(void)
{
    standard_result = legacy_result = child_result = 0;
    standard_count = 17;
    legacy_count = 3;
    scan_count = 0;
    memset(scans, 0, sizeof(scans));
    now_ms = 1000;
}

static void initialize(void)
{
    fixtures();
    handheld_ui_init();
    release();
}

static void page(unsigned first, unsigned selected, unsigned count,
                 unsigned total, const char *name)
{
    const ui_view_t *v = handheld_ui_view();
    if (v->screen != UI_LIBRARY || v->first != first || v->selected != selected ||
            v->item_count != count || v->total != total) {
        printf("FAIL %s page screen=%d first=%u selected=%u count=%u total=%u\n",
               name, (int)v->screen, v->first, v->selected, v->item_count, v->total);
        ++failures;
    }
}

static void check_initialization(void)
{
    fixtures();
    handheld_ui_init();
    page(0, 0, 8, 17, "standard initial page");
    expect(scan_count == 1 && !strcmp(scans[0].path, REGBA_ROM_DIR), "standard root has priority");
    expect(handheld_ui_dirty(), "initial view is dirty");
    handheld_ui_drawn();
    expect(!handheld_ui_dirty(), "draw acknowledgement clears dirty");
    expect_action("A held through init is gated", poll_at(A, 0, 1010), UI_ACTION_NONE);
    expect_action("gated hold stays inert", poll_at(A, 0, 2010), UI_ACTION_NONE);
    page(0, 0, 8, 17, "gate preserves selection");
    release();
    key(DOWN);
    expect_action("A launches after release", tap(A), UI_ACTION_LAUNCH);
    expect(!strcmp(handheld_ui_launch_path(), REGBA_ROM_DIR "/Game01.gba"), "launch path joins standard root");
    char backup[REGBA_RESOURCE_PATH_MAX + 1u];
    expect(regba_backup_path(backup, sizeof(backup), handheld_ui_launch_path()) == 0 &&
           !strcmp(backup, REGBA_SAVE_DIR "/Game01.sav"), "launch path retains native save compatibility");

    fixtures(); standard_count = 0;
    handheld_ui_init();
    page(0, 0, 3, 3, "empty standard falls back to legacy");
    expect(scan_count == 2 && !strcmp(scans[1].path, REGBA_LEGACY_ROM_DIR), "fallback scans legacy once");
    release();
    expect_action("legacy game launch", tap(A), UI_ACTION_LAUNCH);
    expect(!strcmp(handheld_ui_launch_path(), REGBA_LEGACY_ROM_DIR "/Legacy.gba"), "legacy launch path");

    fixtures(); standard_count = legacy_count = 0;
    handheld_ui_init(); release();
    page(0, 0, 0, 0, "both roots empty returns standard");
    expect(scan_count == 3 && !strcmp(handheld_ui_view()->subtitle, REGBA_ROM_DIR), "empty fallback returns to canonical root");
    expect(handheld_ui_view()->message[0] && !handheld_ui_view()->error, "empty directory is explained without IO error");
    expect_action("empty library A refresh", tap(A), UI_ACTION_REFRESH);
    expect_action("unloaded root B stays in library", tap(B), UI_ACTION_NONE);

    fixtures(); standard_result = -ENOENT;
    handheld_ui_init(); release();
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_LEGACY_ROM_DIR), "missing standard also falls back");
    fixtures(); standard_result = legacy_result = -ENODEV;
    handheld_ui_init(); release();
    page(0, 0, 0, 0, "unavailable card has no stale rows");
    expect(handheld_ui_view()->error && handheld_ui_view()->message[0], "card error is visible");
    expect_action("card error offers refresh", tap(A), UI_ACTION_REFRESH);
    standard_result = legacy_result = 0;
    handheld_ui_library(0); release();
    page(0, 0, 8, 17, "refresh recovers readable card");
}

static void check_navigation(void)
{
    initialize();
    key(UP); key(L);
    page(0, 0, 8, 17, "upper boundary clamps");
    for (unsigned i = 0; i < 7; ++i) key(DOWN);
    page(0, 7, 8, 17, "last row on first page");
    key(DOWN);
    page(8, 0, 8, 17, "down crosses page");
    expect(!strcmp(handheld_ui_view()->items[0], "Game08.gba"), "second page entries match global offsets");
    key(R);
    page(16, 0, 1, 17, "R reaches partial last page");
    key(R); key(DOWN);
    page(16, 0, 1, 17, "lower boundary clamps");
    key(L);
    page(8, 0, 8, 17, "L moves back one page");
    key(UP);
    page(0, 7, 8, 17, "up crosses page backwards");
    key(L);
    page(0, 0, 8, 17, "L clamps near start");
    key(R); key(R);
    standard_count = 4;
    handheld_ui_library(0); release();
    page(0, 3, 4, 4, "directory shrink clamps stale selection and rescans");

    initialize();
    key(SELECT);
    page(0, 0, 3, 3, "select toggles legacy root");
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_LEGACY_ROM_DIR), "legacy directory shown");
    key(SELECT);
    page(0, 0, 8, 17, "select toggles back to standard");
}

static void check_repeat(void)
{
    initialize();
    expect_action("initial down", poll_at(DOWN, 0, 100), UI_ACTION_NONE);
    page(0, 1, 8, 17, "initial edge moves once");
    (void)poll_at(DOWN, 0, 449);
    page(0, 1, 8, 17, "repeat waits 350ms");
    (void)poll_at(DOWN, 0, 450);
    page(0, 2, 8, 17, "repeat at 350ms");
    (void)poll_at(DOWN, 0, 549);
    page(0, 2, 8, 17, "repeat waits another 100ms");
    (void)poll_at(DOWN, 0, 550);
    page(0, 3, 8, 17, "repeat at 100ms interval");
    release();
    (void)poll_at(UP, 0, 600);
    page(0, 2, 8, 17, "direction change moves immediately");
    (void)poll_at(UP, 0, 949);
    page(0, 2, 8, 17, "changed direction gets initial delay");
    (void)poll_at(UP, 0, 950);
    page(0, 1, 8, 17, "changed direction repeats");

    initialize();
    uint32_t start = UINT32_MAX - 100u;
    (void)poll_at(DOWN, 0, start);
    (void)poll_at(DOWN, 0, start + 349u);
    page(0, 1, 8, 17, "wrap before repeat deadline");
    (void)poll_at(DOWN, 0, start + 350u);
    page(0, 2, 8, 17, "wrap reaches repeat deadline");
    (void)poll_at(DOWN, 0, start + 449u);
    page(0, 2, 8, 17, "wrap repeat interval before deadline");
    (void)poll_at(DOWN, 0, start + 450u);
    page(0, 3, 8, 17, "wrap repeat interval at deadline");
    release();
    expect_action("A first edge only", poll_at(A, 0, now_ms + 1u), UI_ACTION_LAUNCH);
    expect_action("A hold does not repeat launch", poll_at(A, 0, now_ms + 1000u), UI_ACTION_NONE);
}

static void check_directories(void)
{
    initialize();
    key(A);
    page(0, 0, 2, 2, "A enters subdirectory");
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_ROM_DIR "/RPG"), "child directory path");
    key(A);
    page(0, 0, 1, 1, "A enters nested directory");
    expect_action("nested ROM launch", tap(A), UI_ACTION_LAUNCH);
    expect(!strcmp(handheld_ui_launch_path(), REGBA_ROM_DIR "/RPG/Nested/Deep.gba"), "nested launch path");
    key(B);
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_ROM_DIR "/RPG"), "B returns one directory level");
    key(DOWN);
    expect_action("child ROM launch", tap(A), UI_ACTION_LAUNCH);
    expect(!strcmp(handheld_ui_launch_path(), REGBA_ROM_DIR "/RPG/Child.gba"), "child ROM path");
    key(B);
    page(0, 0, 8, 17, "B returns root");
    key(B);
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_ROM_DIR), "B cannot escape configured root");

    key(SELECT); key(DOWN); key(A);
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_LEGACY_ROM_DIR "/Old"), "legacy subdirectory");
    expect_action("legacy child ROM launch", tap(A), UI_ACTION_LAUNCH);
    expect(!strcmp(handheld_ui_launch_path(), REGBA_LEGACY_ROM_DIR "/Old/Older.gba"), "legacy child launch path");
    key(B);
    expect(!strcmp(handheld_ui_view()->subtitle, REGBA_LEGACY_ROM_DIR), "legacy parent path");

    initialize(); child_result = -EIO;
    key(A);
    page(0, 0, 0, 0, "child read failure clears rows");
    expect(handheld_ui_view()->error, "child read error shown");
    expect_action("child read error refresh", tap(A), UI_ACTION_REFRESH);
    key(B);
    page(0, 0, 8, 17, "B escapes failed child to readable parent");
}

static void pause_at(unsigned selected)
{
    handheld_ui_status(1, 1, 1);
    handheld_ui_pause(REGBA_ROM_DIR "/Game01.gba");
    release();
    for (unsigned i = 0; i < selected; ++i) key(DOWN);
}

static void check_pause_actions(void)
{
    static const struct {
        unsigned row;
        const char *label;
        enum ui_action action;
    } cases[] = {
        {0, "继续游戏", UI_ACTION_RESUME}, {1, "保存进度", UI_ACTION_SAVE},
        {4, "切换游戏", UI_ACTION_SWITCH}, {6, "退出游戏", UI_ACTION_EXIT}
    };
    initialize();
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pause_at(cases[i].row);
        expect(!strcmp(handheld_ui_view()->items[cases[i].row], cases[i].label), "pause label matches requested action");
        expect_action(cases[i].label, tap(A), cases[i].action);
        expect(handheld_ui_view()->game_loaded, "model keeps loaded state until core accepts action");
    }
    pause_at(0); key(UP);
    expect(handheld_ui_view()->item_count == 7 && handheld_ui_view()->selected == 6,
           "seven-row pause menu wraps up to Exit");
    key(DOWN);
    expect(handheld_ui_view()->selected == 0, "pause down wraps to Resume");
    expect_action("pause B resumes", tap(B), UI_ACTION_RESUME);
    pause_at(1);
    expect_action("pause Start activates selection", tap(START), UI_ACTION_SAVE);

    pause_at(0);
    handheld_ui_drawn();
    handheld_ui_status(1, 1, 0);
    expect(handheld_ui_dirty(), "sound result updates view status");
    handheld_ui_drawn();
    handheld_ui_status(1, 1, 0);
    expect(!handheld_ui_dirty(), "unchanged status does not redraw");
    handheld_ui_status(0, 1, 1);
    expect(handheld_ui_dirty() && !handheld_ui_view()->connected, "connection and sound state update");
}

static void check_reload_confirmation(void)
{
    initialize(); pause_at(2);
    expect(!strcmp(handheld_ui_view()->items[2], "读取存档"), "reload entry is visible");
    expect_action("reload selection only opens confirmation",
                  poll_at(A, 0, now_ms + 10u), UI_ACTION_NONE);
    const ui_view_t *v = handheld_ui_view();
    expect(v->screen == UI_NOTICE && !v->error && v->game_loaded &&
           !strcmp(v->title, "读取存档") && strstr(v->message, "未保存") &&
           strstr(v->message, "继续游戏") && !strcmp(v->footer, "A 读取并重启  B 取消"),
           "reload explains discarded progress, restart and in-game Continue");
    expect_action("held launch A cannot also confirm reload",
                  poll_at(A, 0, now_ms + 100u), UI_ACTION_NONE);
    release();
    expect_action("B cancels reload without a core action", tap(B), UI_ACTION_NONE);
    expect(handheld_ui_view()->screen == UI_PAUSE && handheld_ui_view()->game_loaded,
           "cancel keeps original loaded game in pause");
    pause_at(2);
    expect_action("Start also opens reload confirmation", tap(START), UI_ACTION_NONE);
    expect_action("new A confirms reload exactly", tap(A), UI_ACTION_RELOAD);
    expect(handheld_ui_view()->game_loaded, "UI does not unload ROM before core accepts reload");
}

static void check_settings(void)
{
    initialize();
    handheld_ui_options(70, 1, 2);
    pause_at(3);
    expect(!strcmp(handheld_ui_view()->items[3], "游戏设置"), "settings entry is visible");
    expect_action("settings entry opens without a core action",
                  poll_at(A, 0, now_ms + 10u), UI_ACTION_NONE);
    const ui_view_t *v = handheld_ui_view();
    expect(v->screen == UI_SETTINGS && v->item_count == 3 && v->selected == 0 &&
           !strcmp(v->items[0], "音量") && !strcmp(v->items[1], "跳帧") &&
           !strcmp(v->items[2], "运行速度") && !strcmp(v->details[0], "70%") &&
           !strcmp(v->details[1], "跳过 1 帧") && !strcmp(v->details[2], "2 倍"),
           "settings displays the applied volume, skip and speed");
    expect_action("held opening A does not alter volume", poll_at(A, 0, now_ms + 100u), UI_ACTION_NONE);
    release();
    expect_action("volume left", tap(LEFT), UI_ACTION_VOLUME_DOWN);
    expect_action("volume right", tap(RIGHT), UI_ACTION_VOLUME_UP);
    expect_action("volume A", tap(A), UI_ACTION_VOLUME_UP);
    expect_action("volume hold starts once", poll_at(RIGHT, 0, now_ms + 10u), UI_ACTION_VOLUME_UP);
    expect_action("held setting key does not repeat commands", poll_at(RIGHT, 0, now_ms + 500u), UI_ACTION_NONE);
    release();
    key(DOWN);
    expect_action("skip left", tap(LEFT), UI_ACTION_FRAMESKIP_DOWN);
    expect_action("skip right", tap(RIGHT), UI_ACTION_FRAMESKIP_UP);
    expect_action("skip A", tap(A), UI_ACTION_FRAMESKIP_UP);
    handheld_ui_drawn();
    handheld_ui_options(70, 1, 2);
    expect(!handheld_ui_dirty() && v->selected == 1, "unchanged options do not repaint or reset selection");
    handheld_ui_options(60, 3, 4);
    expect(handheld_ui_dirty() && v->selected == 1 && !strcmp(v->details[0], "60%") &&
           !strcmp(v->details[1], "跳过 3 帧") && !strcmp(v->details[2], "4 倍"),
           "applied option readback redraws without resetting selected row");
    key(DOWN);
    expect_action("speed left", tap(LEFT), UI_ACTION_SPEED_DOWN);
    expect_action("speed right", tap(RIGHT), UI_ACTION_SPEED_UP);
    expect_action("speed A", tap(A), UI_ACTION_SPEED_UP);
    handheld_ui_options(0, 0, 1);
    expect(v->selected == 2 && !strcmp(v->details[0], "0%") &&
           !strcmp(v->details[1], "关闭") && !strcmp(v->details[2], "1 倍"),
           "mute/no-skip/normal settings remain unambiguous");
    expect_action("settings B returns without resuming", tap(B), UI_ACTION_NONE);
    expect(v->screen == UI_PAUSE, "settings B returns to pause");
    pause_at(3); key(A);
    expect(v->screen == UI_SETTINGS && v->selected == 2 && !strcmp(v->details[0], "0%"),
           "settings values and selected row survive leaving and reopening");
    key(DOWN);
    expect(v->selected == 0, "settings down wraps to volume");
    key(UP);
    expect(v->selected == 2, "settings up wraps to speed");
    handheld_ui_error("设置失败", -EIO, UI_ACTION_NONE);
    release(); key(B);
    expect(v->screen == UI_SETTINGS && v->selected == 2, "settings notice returns to same selected option");
    expect_action("settings Y returns without resuming", poll_at(0, 1, now_ms + 10u), UI_ACTION_NONE);
    expect(v->screen == UI_PAUSE, "settings Y returns to pause");
    expect_action("held Y cannot also resume", poll_at(0, 1, now_ms + 100u), UI_ACTION_NONE);
    release();
}

static void check_resources(void)
{
    initialize();
    key(START);
    expect(handheld_ui_view()->screen == UI_RESOURCES &&
           handheld_ui_view()->item_count == 5, "Start opens five resource rows");
    expect(!strcmp(handheld_ui_view()->details[0], REGBA_ROM_DIR) &&
           !strcmp(handheld_ui_view()->details[1], REGBA_LEGACY_ROM_DIR) &&
           !strcmp(handheld_ui_view()->details[2], REGBA_SAVE_DIR) &&
           !strcmp(handheld_ui_view()->details[3], REGBA_BIOS_FILE) &&
           !strcmp(handheld_ui_view()->details[4], REGBA_GAME_CONFIG_FILE), "resource paths use production definitions");
    key(B);
    page(0, 0, 8, 17, "resources B returns library");
    expect(!handheld_ui_view()->game_loaded, "resource return preserves unloaded state");
    pause_at(5); key(A);
    expect(handheld_ui_view()->screen == UI_RESOURCES, "pause resource item opens resources");
    key(B);
    expect(handheld_ui_view()->screen == UI_PAUSE && handheld_ui_view()->game_loaded,
           "resources B returns loaded pause menu");
}

static void check_notices(void)
{
    initialize(); pause_at(1);
    handheld_ui_error("保存失败", -EIO, UI_ACTION_SAVE);
    expect(handheld_ui_view()->screen == UI_NOTICE && handheld_ui_view()->error &&
           handheld_ui_view()->game_loaded && handheld_ui_view()->message[0], "save failure notice retains game");
    expect_action("notice gates held A", poll_at(A, 0, now_ms + 10u), UI_ACTION_NONE);
    release();
    expect_action("notice A retries save", tap(A), UI_ACTION_SAVE);
    handheld_ui_notice("保存完成", "进度已保存。", 0, UI_ACTION_NONE);
    release(); key(B);
    expect(handheld_ui_view()->screen == UI_PAUSE && handheld_ui_view()->game_loaded,
           "replacement notice B retains pause parent");

    initialize();
    handheld_ui_error("加载失败", -ENOENT, UI_ACTION_LAUNCH);
    release();
    expect_action("notice launch retry", tap(A), UI_ACTION_LAUNCH);
    key(B);
    page(0, 0, 8, 17, "unloaded notice B returns library");
    expect(!handheld_ui_view()->game_loaded, "failed load does not invent loaded game");
    handheld_ui_notice("提示", "没有重试操作。", 0, UI_ACTION_NONE);
    release(); key(A);
    page(0, 0, 8, 17, "no-retry notice A returns parent");

    handheld_ui_status(1, 1, 1);
    handheld_ui_library(1); release();
    handheld_ui_error("切换失败", -ENOMEM, UI_ACTION_SWITCH);
    release(); key(B);
    expect(handheld_ui_view()->screen == UI_LIBRARY && handheld_ui_view()->game_loaded,
           "library notice retains previously loaded game");
    expect_action("loaded root B resumes", tap(B), UI_ACTION_RESUME);

    initialize(); pause_at(2);
    handheld_ui_status(1, 0, 1); /* Core reports that a failed load left no ROM. */
    handheld_ui_error("加载失败", -EFBIG, UI_ACTION_LAUNCH);
    release(); key(B);
    page(0, 0, 8, 17, "pause notice cannot return to an unloaded game");
    expect(!handheld_ui_view()->game_loaded, "core unloaded state survives notice return");

    initialize(); key(START);
    handheld_ui_notice("提示", "资源路径说明", 0, UI_ACTION_NONE);
    release(); key(B);
    expect(handheld_ui_view()->screen == UI_RESOURCES,
           "notice B restores resource parent");
    key(B);
    page(0, 0, 8, 17, "resource notice roundtrip retains library parent");
}

static void check_menu_button(void)
{
    initialize();
    expect_action("Y with no game has no action", poll_at(0, 1, now_ms + 10u), UI_ACTION_NONE);
    expect(handheld_ui_view()->screen == UI_LIBRARY, "Y does not invent loaded state");
    release();
    pause_at(0);
    expect_action("fresh Y edge resumes", poll_at(0, 1, now_ms + 10u), UI_ACTION_RESUME);
    expect_action("held Y does not repeat resume", poll_at(0, 1, now_ms + 500u), UI_ACTION_NONE);
    handheld_ui_pause(REGBA_ROM_DIR "/Game01.gba");
    expect_action("reopen waits for Y release", poll_at(0, 1, now_ms + 500u), UI_ACTION_NONE);
    expect_action("release A while Y held is insufficient", poll_at(A, 1, now_ms + 10u), UI_ACTION_NONE);
    expect_action("release Y while A held is insufficient", poll_at(A, 0, now_ms + 10u), UI_ACTION_NONE);
    release();
    expect_action("new Y after all released resumes", poll_at(0, 1, now_ms + 10u), UI_ACTION_RESUME);
    release();
    handheld_ui_library(1); release();
    expect_action("loaded library Y opens pause", poll_at(0, 1, now_ms + 10u), UI_ACTION_NONE);
    expect(handheld_ui_view()->screen == UI_PAUSE, "Y event selects pause without GBA key bits");
    expect_action("opening Y cannot immediately resume", poll_at(0, 1, now_ms + 50u), UI_ACTION_NONE);
    release();
    expect_action("Y released then pressed resumes", poll_at(0, 1, now_ms + 10u), UI_ACTION_RESUME);
}

static void check_utf8_subtitle(void)
{
    initialize();
    /* 2 ASCII bytes + 100 three-byte characters exceed the 256-byte view.
     * The visible prefix fits exactly 84 complete Chinese characters. */
    char filename[307], expected[255];
    memcpy(filename, "AB", 2); memcpy(expected, "AB", 2);
    for (unsigned i = 0; i < 100; ++i) memcpy(filename + 2u + i * 3u, u8"汉", 3);
    memcpy(filename + 302, ".gba", 5);
    for (unsigned i = 0; i < 84; ++i) memcpy(expected + 2u + i * 3u, u8"汉", 3);
    expected[254] = 0;
    handheld_ui_pause(filename);
    expect(handheld_ui_view()->screen == UI_PAUSE &&
           !strcmp(handheld_ui_view()->subtitle, expected),
           "long Chinese pause title ends on a complete UTF-8 character");
}

int main(void)
{
    check_initialization();
    check_navigation();
    check_repeat();
    check_directories();
    check_pause_actions();
    check_reload_confirmation();
    check_settings();
    check_resources();
    check_notices();
    check_menu_button();
    check_utf8_subtitle();
    if (failures) {
        printf("handheld UI regression: %u failure(s)\n", failures);
        return 1;
    }
    puts("handheld UI regression PASS: roots, release gate, launch paths, bounds/pages, repeat/wrap, directories, resources, pause actions, confirmed reload, settings/readback, notices, Y edges");
    return 0;
}
