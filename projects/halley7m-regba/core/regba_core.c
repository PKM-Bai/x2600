/* X2600 FreeRTOS handheld frontend for ReGBA. GPL-2.0-or-later. */
#include "common.h"
#include "regba_core.h"
#include "test_resources.h"
#include "platform_runtime.h"
#include "storage.h"
#include "audio.h"
#include "game_stats.h"
#include "video.h"
#include "buttons.h"
#include "bt_gamepad.h"
#include "battery_save.h"
#include "handheld_ui.h"
#include "playback.h"
#include "resources.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdarg.h>
#include <setjmp.h>

static uint16_t screen[240 * 160] __attribute__((aligned(64)));
uint16_t *GBAScreen = screen;
uint32_t GBAScreenPitch = 240;
static QueueHandle_t commands;
/* GCC -fstack-usage: one ARM/Thumb translator frame is 10472/10440 bytes.
 * Static branch linking recursively translates additional blocks. The old
 * 32KiB task stack could not fit even three levels plus task/IRQ context.
 * Keep the word count below FreeRTOS's uint16_t stack-depth limit. */
enum { CORE_STACK_WORDS = 60 * 1024, CORE_STACK_BYTES = CORE_STACK_WORDS * sizeof(StackType_t) };
static TaskHandle_t core_handle;
static uintptr_t core_last_sp;
extern uint32_t translation_recursion_level;
enum command_type { CMD_LOAD, CMD_RUN, CMD_TEST, CMD_SAVE, CMD_TONE, CMD_VIDEO, CMD_MENU };
struct command { enum command_type type; uint32_t frames; char path[MAX_PATH]; };
static volatile uint32_t running, stop_requested, requested_keys, want_audio;
static volatile uint32_t audio_stop_pending;
static volatile uint32_t command_busy;
static volatile uint32_t rendered_frames, frame_crc;
static uint32_t drawn_frames;
static unsigned playback_frameskip, playback_speed = 1;
static volatile uint32_t gui_enabled, gui_showing, gui_requested, gui_input_gate;
static int gui_initialized, gui_storage_ready, gui_last_menu, last_flush_result;
static int gui_last_result;
static uint32_t frame_limit, boundary_pending;
static uint64_t start_us, next_frame_us;
static jmp_buf return_to_task;
static bool jump_valid, sound_started;
static volatile uint32_t save_requested;
static bool backup_dirty, save_in_flight;
static uint64_t backup_changed_us, save_attempt_us;
extern uint32_t backup_update;
extern const uint32_t write_backup_delay;
extern uint32_t save_backup(void);
extern int regba_reload_backup(void);
extern bool IsZippedROM;
extern uint32_t gamepak_ram_pages, gamepak_size;
static FILE *mapped_file;
static int16_t pending_pcm[256 * 2];
static size_t pcm_offset, pcm_frames;
static uint64_t audio_update_last_us;
static uint32_t audio_update_max_gap_us, audio_update_gap_native, audio_update_gap_pending;
extern bool IsNintendoBIOS;
extern uint8_t nintendo_bios_sha1[];
static void gui_paint(void);
static void gui_progress(unsigned percent);
static int core_load_game(const char *path);
static void gui_service(void);

static int gamepad_menu_down(void)
{
    bt_gamepad_stats_t pad;
    bt_gamepad_get_stats(&pad);
    return pad.connected && pad.data_valid && !pad.stale && (pad.key2 & BT_GAMEPAD_KEY2_Y);
}

static void core_stack_diagnostic(const char *where)
{
    __asm__ volatile("move %0,$sp" : "=r"(core_last_sp));
    printf("core stack %s frame=%lu sp=%08lx min_unused=%lu/%u translation_depth=%lu peak=%lu\n",
           where, (unsigned long)rendered_frames, (unsigned long)core_last_sp,
           (unsigned long)uxTaskGetStackHighWaterMark(core_handle) * sizeof(StackType_t),
           (unsigned)CORE_STACK_BYTES, (unsigned long)translation_recursion_level,
           (unsigned long)translation_recursion_peak);
}

static uint32_t crc32_bytes(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = ~0u;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Only the core task reads guest backup RAM. The worker owns a snapshot while
 * it writes the card, so even a 128KiB save cannot block the PCM producer. */
static void observe_backup(void)
{
    if (backup_update == write_backup_delay) {
        backup_update = write_backup_delay + 1;
        backup_dirty = true;
        backup_changed_us = platform_time_us();
    }
    if (save_in_flight && !battery_save_busy()) {
        save_in_flight = false;
        if (battery_save_result() < 0) {
            backup_dirty = true;
            save_attempt_us = platform_time_us();
        }
    }
}

static int submit_backup(void)
{
    errno = 0;
    if (!save_backup()) return errno ? -errno : -ENODATA;
    backup_dirty = false;
    save_requested = 0;
    save_in_flight = true;
    save_attempt_us = platform_time_us();
    return 0;
}

static void service_backup(void)
{
    observe_backup();
    uint64_t now = platform_time_us();
    if (save_in_flight || battery_save_busy()) return;
    if (save_requested || (backup_dirty && now - backup_changed_us >= 2000000u &&
                          now - save_attempt_us >= 2000000u)) {
        int result = submit_backup();
        if (result < 0) {
            save_requested = 0;
            save_attempt_us = now;
            printf("SAVE request failed result=%d\n", result);
        }
    }
}

static int flush_backup(void)
{
    observe_backup();
    if (save_in_flight) {
        int result = battery_save_wait();
        save_in_flight = false;
        if (result < 0) backup_dirty = true;
    }
    if (!backup_dirty && !save_requested) return 0;
    int result = submit_backup();
    if (result < 0) { save_requested = 0; return result; }
    result = battery_save_wait();
    save_in_flight = false;
    if (result < 0) backup_dirty = true;
    return result;
}

/* Opening a menu waits for an already owned snapshot, but does not replace
 * the on-card save before the user can choose to read it. */
static int settle_backup(void)
{
    observe_backup();
    if (!save_in_flight && !battery_save_busy()) return 0;
    int result = battery_save_wait();
    save_in_flight = false;
    if (result < 0) backup_dirty = true;
    return result;
}

static int core_reload_save(void)
{
    if (!IsGameLoaded) return -ENOENT;
    int result = settle_backup();
    if (result < 0) return result;
    result = regba_reload_backup();
    if (result < 0) return result;
    backup_update = write_backup_delay + 1;
    backup_dirty = save_in_flight = false;
    save_requested = 0;
    backup_changed_us = save_attempt_us = 0;
    pcm_frames = pcm_offset = 0;
    printf("SAVE reloaded cached_rom=1 path=%s\n", CurrentGamePath);
    return 0;
}

FILE *regba_fopen(const char *path, const char *mode)
{
    if (!strcmp(path, "/__selftest.gba") && !strcmp(mode, "rb"))
        return fmemopen((void *)selftest_rom, sizeof(selftest_rom), "rb");
    if (!strcmp(mode, "rb")) {
        if (!strcmp(path, REGBA_BUILTIN_GAME_CONFIG_FILE))
            return fmemopen((void *)bundled_game_config, sizeof(bundled_game_config), "rb");
        if (!strcmp(path, "/__save_sram.gba"))
            return fmemopen((void *)save_sram_rom, sizeof(save_sram_rom), "rb");
        if (!strcmp(path, "/__save_flash.gba"))
            return fmemopen((void *)save_flash_rom, sizeof(save_flash_rom), "rb");
        if (!strcmp(path, "/__save_eeprom.gba"))
            return fmemopen((void *)save_eeprom_rom, sizeof(save_eeprom_rom), "rb");
    }
    return fopen(path, mode);
}

void ReGBA_Trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}

int regba_jit_test(void)
{
    static uint32_t code[16] __attribute__((aligned(64)));
    typedef int (*function_t)(void);
    for (unsigned value = 37; value <= 73; value += 36) {
        code[0] = 0x24020000 | value; /* addiu v0,zero,value */
        code[1] = 0x03E00008;       /* jr ra */
        code[2] = 0;
        ReGBA_MakeCodeVisible(code, sizeof(code));
        if (((function_t)code)() != (int)value) return -1;
        vTaskDelay(pdMS_TO_TICKS(2));
        if (((function_t)code)() != (int)value) return -1;
    }
    puts("jit-test PASS: same-address rewrite 37 -> 73 across ticks");
    return 0;
}

void ReGBA_BadJump(uint32_t from, uint32_t to)
{
    printf("core bad jump %08lx -> %08lx\n", (unsigned long)from, (unsigned long)to);
    if (jump_valid) longjmp(return_to_task, 2);
    platform_panic("ReGBA bad jump");
}
void ReGBA_MaxBlockExitsReached(uint32_t a, uint32_t b, uint32_t count)
{ printf("JIT exits %08lx..%08lx %lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)count); }
void ReGBA_MaxBlockSizeReached(uint32_t a, uint32_t b, uint32_t count)
{ printf("JIT size %08lx..%08lx %lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)count); }

static int16_t clamp12(int32_t value)
{ return value < -2048 ? -2048 : value > 2047 ? 2047 : (int16_t)value; }

static void reset_audio_update_timing(void)
{
    taskENTER_CRITICAL();
    audio_update_last_us = 0;
    audio_update_max_gap_us = audio_update_gap_native = audio_update_gap_pending = 0;
    taskEXIT_CRITICAL();
}

signed int ReGBA_AudioUpdate(void)
{
    if (!sound_started) {
        audio_update_last_us = 0;
        ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable());
        return 0;
    }
    /* Both update_gbc_sound and frame boundaries enter here. Measure the
     * common producer-call gap before consuming any currently available PCM.
     */
    uint64_t now = platform_time_us();
    uint64_t gap = audio_update_last_us ? now - audio_update_last_us : 0;
    audio_update_last_us = now;
    if (gap > audio_update_max_gap_us) {
        taskENTER_CRITICAL();
        audio_update_max_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        audio_update_gap_native = ReGBA_GetAudioSamplesAvailable();
        audio_update_gap_pending = (uint32_t)(pcm_frames - pcm_offset);
        taskEXIT_CRITICAL();
    }
    /* Keep 512 native frames ahead, matching the core's delayed sound writes. */
    for (unsigned chunk = 0; chunk < 8; ++chunk) {
        if (pcm_offset < pcm_frames) {
            pcm_offset += audio_submit_stereo(pending_pcm + pcm_offset * 2,
                                              pcm_frames - pcm_offset, 44100);
            if (pcm_offset < pcm_frames) return 0;
        }
        if (ReGBA_GetAudioSamplesAvailable() < 1024) return 0;
        for (unsigned i = 0; i < 256; ++i) {
            int16_t l1, r1, l2, r2;
            ReGBA_LoadNextAudioSample(&l1, &r1);
            ReGBA_LoadNextAudioSample(&l2, &r2);
            pending_pcm[2*i] = ((int32_t)clamp12(l1) + clamp12(l2)) / 2 * 16;
            pending_pcm[2*i+1] = ((int32_t)clamp12(r1) + clamp12(r2)) / 2 * 16;
        }
        pcm_offset = 0;
        pcm_frames = 256;
    }
    return 0;
}

void ReGBA_RenderScreen(void)
{
    int draw = playback_should_render(rendered_frames, playback_frameskip);
    ++rendered_frames;
    if (draw) {
        ++drawn_frames;
        if (drawn_frames == 1 || drawn_frames % 60 == 0)
            frame_crc = crc32_bytes(screen, sizeof(screen));
        video_present_gba(screen, GBAScreenPitch);
    }
    boundary_pending = 1;
}
bool ReGBA_IsRenderingNextFrame(void)
{ return playback_should_render(rendered_frames, playback_frameskip) != 0; }
void ReGBA_DisplayFPS(void) { }

/* Called after update_gba has committed VCOUNT/IRQ/timer state. Never in ISR. */
void regba_frame_boundary(void)
{
    if (!boundary_pending) return;
    boundary_pending = 0;
    game_stats_frame(rendered_frames);
    if (stop_requested) longjmp(return_to_task, 1);
    taskENTER_CRITICAL();
    uint32_t stopped_audio = audio_stop_pending;
    audio_stop_pending = 0;
    taskEXIT_CRITICAL();
    if (stopped_audio) {
        /* An off/on pair may both arrive between frames. The console has
         * already stopped DMA; reconcile the producer before the latest on. */
        audio_stop();
        sound_started = false;
        pcm_frames = pcm_offset = 0;
    }
    int play_audio = want_audio && playback_speed == 1;
    if (play_audio && !sound_started) {
        pcm_frames = pcm_offset = 0;
        ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable());
        uint64_t before = platform_time_us();
        int result = audio_start();
        if (result == 0) {
            reset_audio_update_timing();
            sound_started = true;
        }
        /* A console off/on during settling cancels this start. Keep the
         * latest request: off is already zero; a later on retries next frame. */
        else if (result != -ECANCELED) want_audio = 0;
        start_us += platform_time_us() - before; /* codec power-up is not guest time */
    } else if (!play_audio && sound_started) {
        audio_stop(); sound_started = false; pcm_frames = pcm_offset = 0;
    }
    ReGBA_AudioUpdate();
    if (gui_enabled) {
        int menu = gamepad_menu_down();
        if (menu && !gui_last_menu) {
            gui_requested = 1;
            stop_requested = 1;
        }
        gui_last_menu = menu;
    }
    if (!gui_requested) service_backup();
    if (stop_requested || (frame_limit && rendered_frames >= frame_limit))
        longjmp(return_to_task, 1);
    /* 280896 GBA cycles/frame at 16777216Hz, preserve the fractional part. */
    next_frame_us = start_us + playback_target_us(rendered_frames, playback_speed);
    while (platform_time_us() < next_frame_us) {
        if (stop_requested) longjmp(return_to_task, 1);
        vTaskDelay(1);
    }
}

enum ReGBA_Buttons ReGBA_GetPressedButtons(void)
{
    uint16_t keys = (requested_keys | buttons_get_keys() | bt_gamepad_get_keys()) & 0x3FF;
    if (gui_input_gate) {
        if (!keys) gui_input_gate = 0;
        return 0;
    }
    return keys;
}
uint32_t ReGBA_Menu(enum ReGBA_MenuEntryReason reason) { (void)reason; stop_requested = 1; return 0; }
void ReGBA_LoadRTCTime(struct ReGBA_RTC *rtc)
{ *rtc = (struct ReGBA_RTC){ .year = 26, .month = 1, .day = 1, .weekday = 4 }; }

bool ReGBA_GetBackupFilename(char *result, const char *path)
{
    return regba_backup_path(result, MAX_PATH + 1, path) == 0;
}
bool ReGBA_GetSavedStateFilename(char *out, const char *path, uint32_t slot)
{ (void)out; (void)path; (void)slot; return false; }
bool ReGBA_GetBundledGameConfig(char *out) { strcpy(out, REGBA_BUILTIN_GAME_CONFIG_FILE); return true; }
size_t FILE_LENGTH(FILE *file)
{
    long at = ftell(file);
    if (at < 0 || fseek(file, 0, SEEK_END)) return 0;
    long end = ftell(file);
    if (fseek(file, at, SEEK_SET) || end < 0) return 0;
    return end;
}
uint8_t *ReGBA_MapEntireROM(FILE *file, size_t size)
{
    if (size < 0xC0 || size > 32u * 1024 * 1024) { errno = EFBIG; return NULL; }
    size_t rounded = (size + 32767) & ~32767u;
    if (platform_free_heap() < rounded + 4u * 1024 * 1024) { errno = ENOMEM; return NULL; }
    uint8_t *rom = malloc(rounded);
    if (!rom) return NULL;
    memset(rom, 0xFF, rounded);
    if (fseek(file, 0, SEEK_SET)) { free(rom); errno = EIO; return NULL; }
    uint64_t progress_at = 0;
    for (size_t offset = 0; offset < size;) {
        size_t amount = size - offset;
        if (amount > 65536u) amount = 65536u;
        if (fread(rom + offset, 1, amount, file) != amount) {
            free(rom); errno = EIO; return NULL;
        }
        offset += amount;
        if (gui_enabled) {
            uint64_t now = platform_time_us();
            if (offset == size || now >= progress_at) {
                gui_progress((unsigned)(offset * 100u / size));
                progress_at = now + 100000u;
            }
        }
    }
    mapped_file = file;
    return rom;
}
void ReGBA_UnmapEntireROM(void *rom) { if (mapped_file) fclose(mapped_file); mapped_file = NULL; free(rom); }
uint8_t *ReGBA_AllocateROM(size_t size) { (void)size; errno = ENOTSUP; return NULL; }
size_t ReGBA_AllocateOnDemandBuffer(void **buffer) { *buffer = NULL; return 0; }
void ReGBA_DeallocateROM(void *buffer) { free(buffer); }
ssize_t load_file_zip(const char *path, uint8_t **buffer)
{ (void)path; *buffer = NULL; errno = ENOTSUP; puts("ZIP unsupported: use raw .gba"); return -1; }
void ReGBA_OnGameLoaded(const char *path) { printf("loaded %s (%lu bytes padded)\n", path, (unsigned long)gamepak_ram_buffer_size); }
void ReGBA_ProgressInitialise(enum ReGBA_FileAction action) { (void)action; }
void ReGBA_ProgressUpdate(uint32_t current, uint32_t total) { (void)current; (void)total; }
void ReGBA_ProgressFinalise(void) { }
bool ReGBA_SaveSettings(char *name, bool pergame) { (void)name; (void)pergame; return false; }
void ReGBA_LoadSettings(char *name, bool pergame) { (void)name; (void)pergame; }
void get_ticks_us(u64 *value) { *value = platform_time_us(); }
void delay_us(u32 us) { if (us) vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000)); }
void synchronize(void) { }
void quit(void) { stop_requested = 1; }
void error_quit(void) { platform_panic("ReGBA error"); }
extern void init_main(void);
void reset_gba(void)
{
    init_main(); init_memory(); init_cpu(0); reset_sound();
    reset_audio_update_timing();
}

static bool run_game(uint32_t frames, bool selftest)
{
    if (!IsGameLoaded) { puts("No ROM loaded"); return false; }
    frame_limit = frames;
    rendered_frames = drawn_frames = frame_crc = boundary_pending = stop_requested = 0;
    pcm_frames = pcm_offset = 0;
    reset_audio_update_timing();
    ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable());
    start_us = platform_time_us();
    running = jump_valid = true;
    core_stack_diagnostic("before-run");
    game_stats_begin();
    int reason = setjmp(return_to_task);
    if (reason == 0) {
        reg[CHANGED_PC_STATUS] = 1;
        execute_arm_translate(execute_cycles);
    }
    jump_valid = false;
    game_stats_end();
    core_stack_diagnostic("after-run");
    if (sound_started) { audio_stop(); sound_started = false; }
    frame_crc = crc32_bytes(screen, sizeof(screen));
    uint64_t elapsed = platform_time_us() - start_us;
    printf("core stopped reason=%d frames=%lu elapsed_us=%llu RGB555_CRC32=%08lx\n",
           reason, (unsigned long)rendered_frames, (unsigned long long)elapsed, (unsigned long)frame_crc);
    bool passed = reason == 1 && rendered_frames == frames;
    if (selftest) {
        const uint32_t expected[] = {42, 55, 66, 77, 6, 0x600D};
        bool pass = passed;
        for (unsigned i = 0; i < 6; ++i) {
            uint32_t value; memcpy(&value, ewram_data + i * 4, 4);
            printf("mailbox[%u]=%08lx expected=%08lx\n", i, (unsigned long)value, (unsigned long)expected[i]);
            pass &= value == expected[i];
        }
        for (unsigned i = 0; i < 240 * 160; ++i) pass &= screen[i] == 0x3DEF;
        uint32_t irq_count; memcpy(&irq_count, ewram_data + 24, 4);
        pass &= irq_count > 1;
        printf("VBlank IRQ count=%lu\n", (unsigned long)irq_count);
        printf("core-test %s (ARM/Thumb/SWI/IRQ/RAM-code/SMC/frame pixels)\n", pass ? "PASS" : "FAIL");
        passed = pass;
    }
    last_flush_result = gui_requested ? settle_backup() : flush_backup();
    if (last_flush_result < 0)
        printf("SAVE stop flush failed result=%d; backup retained in RAM\n", last_flush_result);
    /* A stopped state is published only after its final snapshot and sync. */
    running = 0;
    return passed;
}

static int core_load_game(const char *path)
{
    if (!path || strlen(path) >= MAX_PATH) return -ENAMETOOLONG;
    int saved = flush_backup();
    if (saved < 0) {
        printf("load cancelled: unsaved backup result=%d; retry save first\n", saved);
        return saved;
    }
    memcpy(bios.rom, openbios, sizeof(openbios));
    FILE *file = fopen(REGBA_BIOS_FILE, "rb");
    if (file) {
        if (FILE_LENGTH(file) == 16384) {
            size_t got = fread(bios.rom, 1, 16384, file);
            if (got != 16384) memcpy(bios.rom, openbios, sizeof(openbios));
            else puts("Using TF BIOS (skip boot animation)");
        }
        fclose(file);
    }
    sha1nfo sha1;
    sha1_init(&sha1);
    sha1_write(&sha1, bios.rom, 16384);
    IsNintendoBIOS = !memcmp(sha1_result(&sha1), nintendo_bios_sha1, 20);
    int result = load_gamepak(path);
    int load_errno = result ? (errno ? errno : ENOEXEC) : 0;
    backup_update = write_backup_delay + 1;
    backup_dirty = save_in_flight = false;
    save_requested = 0;
    backup_changed_us = save_attempt_us = 0;
    printf("load result=%d errno=%d heap=%lu\n", result, load_errno, (unsigned long)platform_free_heap());
    return -load_errno;
}

static void gui_paint(void)
{
    if (gui_showing && handheld_ui_dirty()) {
        int result = video_present_ui(ui_draw, handheld_ui_view());
        if (!result) handheld_ui_drawn();
    }
}

static void gui_message(const char *title, const char *text)
{
    handheld_ui_notice(title, text, 0, UI_ACTION_NONE);
    uint64_t deadline = platform_time_us() + 100000u;
    do {
        gui_paint();
        if (!handheld_ui_dirty()) break;
        vTaskDelay(1);
    } while (platform_time_us() < deadline);
}

static void gui_progress(unsigned percent)
{
    ui_view_t *view = handheld_ui_view();
    snprintf(view->message, sizeof(view->message), "正在读取游戏  %u%%\n请稍候", percent);
    /* A notice marks the view dirty; preserving its parent keeps Back useful
     * if reading fails. Drawing remains in this emulator task. */
    handheld_ui_notice("正在加载", view->message, 0, UI_ACTION_NONE);
    gui_paint();
}

static void gui_run_game(void)
{
    gui_showing = gui_requested = 0;
    gui_input_gate = 1;
    gui_last_menu = gamepad_menu_down();
    run_game(0, false);
    gui_showing = 1;
    gui_requested = 0;
    handheld_ui_pause(CurrentGamePath);
    gui_last_result = last_flush_result;
    if (last_flush_result < 0)
        handheld_ui_error("保存未完成", last_flush_result, UI_ACTION_SAVE);
}

static void gui_open(void)
{
    gui_enabled = gui_showing = 1;
    gui_requested = 0;
    int video_result = video_init();
    if (video_result < 0) {
        gui_last_result = video_result;
        printf("menu display result=%d\n", video_result);
        return;
    }
    if (!gui_initialized) {
        if (!IsGameLoaded) want_audio = 1;
        /* Allocate the persistent audio task/stack before the first large ROM.
         * Creating it above a mapped ROM splits the heap after that ROM is
         * freed and can prevent a later 32MiB game from fitting. Init is silent;
         * the normal playback path can retry if hardware setup fails here. */
        (void)audio_init();
        /* Reuse an existing mount: a loaded ROM intentionally keeps its
         * mapped file open, so remounting would be rejected by the owner. */
        int result = storage_prepare_dirs();
        if (result == -ENODEV || result == -ENXIO) {
            result = storage_mount();
            if (!result) result = storage_prepare_dirs();
        }
        gui_storage_ready = result == 0;
        handheld_ui_init();
        gui_initialized = 1;
        if (result < 0) handheld_ui_error("TF 卡未就绪", result, UI_ACTION_REFRESH);
    }
    if (IsGameLoaded) handheld_ui_pause(CurrentGamePath);
    else if (gui_storage_ready) handheld_ui_library(0);
    gui_paint();
}

static void gui_unload_game(void)
{
    /* This port accepts raw, entirely mapped ROMs. Its FILE is released by
     * the same unmap hook used by the native loader. Save has completed. */
    if (IsGameLoaded) ReGBA_UnmapEntireROM(gamepak_rom);
    gamepak_rom = NULL;
    IsGameLoaded = IsZippedROM = false;
    gamepak_ram_buffer_size = gamepak_ram_pages = gamepak_size = 0;
    CurrentGamePath[0] = 0;
    backup_dirty = save_in_flight = false;
    save_requested = 0;
}

static int gui_check_rom(const char *path)
{
    if (!path || strlen(path) >= MAX_PATH) return -ENAMETOOLONG;
    struct stat info;
    if (stat(path, &info) != 0) return -errno;
    if (info.st_size < 0xc0 || info.st_size > 32u * 1024 * 1024) return -EFBIG;
    char backup_path[MAX_PATH + 1];
    int result = regba_backup_path(backup_path, sizeof(backup_path), path);
    if (result < 0) return result;
    FILE *file = fopen(path, "rb");
    if (!file) return -errno;
    uint8_t magic[4];
    size_t got = fread(magic, 1, sizeof(magic), file);
    int closed = fclose(file);
    if (got != sizeof(magic) || closed) return -EIO;
    return magic[3] == 0xea ? 0 : -ENOEXEC;
}

static void gui_service(void)
{
    if (!gui_enabled || !gui_showing) return;
    bt_gamepad_stats_t pad;
    bt_gamepad_get_stats(&pad);
    handheld_ui_status(pad.connected, IsGameLoaded, want_audio);
    handheld_ui_options(audio_get_volume(), playback_frameskip, playback_speed);
    uint16_t keys = (requested_keys | buttons_get_keys() | pad.keys) & 0x3ffu;
    int menu = pad.connected && pad.data_valid && !pad.stale && (pad.key2 & BT_GAMEPAD_KEY2_Y);
    enum ui_action action = handheld_ui_poll(keys, menu, (uint32_t)(platform_time_us() / 1000u));
    int result = 0;
    if (action == UI_ACTION_NONE) { gui_paint(); return; }
    command_busy = 1;
    if (action == UI_ACTION_RESUME && IsGameLoaded) {
        gui_run_game();
        result = last_flush_result;
    } else if (action == UI_ACTION_LAUNCH) {
        const char *path = handheld_ui_launch_path();
        result = gui_check_rom(path);
        if (!result) {
            gui_message("正在保存", "正在完成当前游戏的保存与同步");
            result = flush_backup();
        }
        if (!result) {
            gui_message("正在加载", "正在读取游戏  0%\n请稍候");
            result = core_load_game(path);
        }
        if (result < 0) {
            handheld_ui_status(pad.connected, IsGameLoaded, want_audio);
            handheld_ui_error("无法启动游戏", result, UI_ACTION_LAUNCH);
        } else {
            gui_run_game();
            result = last_flush_result;
        }
    } else if (action == UI_ACTION_SAVE) {
        gui_message("正在保存", "正在将游戏进度写入 TF 卡");
        save_requested = IsGameLoaded;
        result = IsGameLoaded ? flush_backup() : -ENOENT;
        if (result < 0) handheld_ui_error("保存未完成", result, UI_ACTION_SAVE);
        else handheld_ui_notice("保存成功", "游戏进度已保存并同步到 TF 卡。", 0, UI_ACTION_NONE);
    } else if (action == UI_ACTION_RELOAD) {
        gui_message("正在读取存档", "读取完成后会重新启动游戏，请在游戏内选择继续");
        result = core_reload_save();
        if (result < 0) handheld_ui_error("无法读取存档", result, UI_ACTION_RELOAD);
        else {
            gui_run_game();
            result = last_flush_result;
        }
    } else if (action == UI_ACTION_VOLUME_DOWN || action == UI_ACTION_VOLUME_UP) {
        unsigned volume = audio_get_volume();
        if (action == UI_ACTION_VOLUME_DOWN) volume = volume >= 10 ? volume - 10 : 0;
        else volume = volume <= 90 ? volume + 10 : 100;
        result = audio_set_volume(volume);
    } else if (action == UI_ACTION_FRAMESKIP_DOWN || action == UI_ACTION_FRAMESKIP_UP) {
        playback_frameskip = (playback_frameskip + (action == UI_ACTION_FRAMESKIP_UP ? 1u : 3u)) % 4u;
    } else if (action == UI_ACTION_SPEED_DOWN || action == UI_ACTION_SPEED_UP) {
        if (action == UI_ACTION_SPEED_UP) playback_speed = playback_speed == 4 ? 1 : playback_speed * 2;
        else playback_speed = playback_speed == 1 ? 4 : playback_speed / 2;
    } else if (action == UI_ACTION_SWITCH || action == UI_ACTION_EXIT) {
        gui_message("正在保存", "正在完成保存与同步，请稍候");
        result = flush_backup();
        if (result < 0) handheld_ui_error("保存未完成", result, action);
        else {
            if (action == UI_ACTION_EXIT) gui_unload_game();
            handheld_ui_library(IsGameLoaded);
        }
    } else if (action == UI_ACTION_SOUND) {
        want_audio = !want_audio;
        handheld_ui_status(pad.connected, IsGameLoaded, want_audio);
    } else if (action == UI_ACTION_REFRESH) {
        if (!gui_storage_ready) {
            result = storage_mount();
            if (!result) result = storage_prepare_dirs();
            gui_storage_ready = result == 0;
        }
        if (result < 0) handheld_ui_error("TF 卡未就绪", result, UI_ACTION_REFRESH);
        else handheld_ui_library(IsGameLoaded);
    }
    gui_last_result = result;
    handheld_ui_options(audio_get_volume(), playback_frameskip, playback_speed);
    command_busy = 0;
    gui_paint();
}

static void core_task(void *arg)
{
    (void)arg;
    strcpy(main_path, REGBA_CONFIG_DIR);
    memcpy(bios.rom, openbios, sizeof(openbios));
    init_sound();
    struct command command;
    for (;;) {
        if (xQueueReceive(commands, &command, gui_enabled && gui_showing ? pdMS_TO_TICKS(20) : portMAX_DELAY) != pdTRUE) {
            gui_service();
            continue;
        }
        if (command.type != CMD_MENU && command.type != CMD_SAVE) {
            gui_enabled = gui_showing = 0;
        }
        if (command.type == CMD_LOAD || command.type == CMD_TEST) {
            int result = core_load_game(command.type == CMD_TEST ? "/__selftest.gba" : command.path);
            if (!result && command.type == CMD_TEST) {
                bool before_pass = run_game(120, true);
                uint32_t before_crc = frame_crc;
                uint32_t before_irq, after_irq;
                memcpy(&before_irq, ewram_data + 24, sizeof(before_irq));
                /* FULL_CACHE clears ROM/BIOS metadata and cascades to the
                 * writable cache; resume this exact guest state afterwards.
                 */
                flush_translation_cache(TRANSLATION_REGION_READONLY, FLUSH_REASON_FULL_CACHE);
                bool after_pass = run_game(120, true);
                memcpy(&after_irq, ewram_data + 24, sizeof(after_irq));
                printf("core-retranslate-test %s before=%08lx after=%08lx IRQ=%lu->%lu\n",
                       before_pass && after_pass && before_crc == frame_crc && after_irq > before_irq ? "PASS" : "FAIL",
                       (unsigned long)before_crc, (unsigned long)frame_crc,
                       (unsigned long)before_irq, (unsigned long)after_irq);
            }
        } else if (command.type == CMD_RUN) {
            run_game(command.frames, false);
            if (gui_requested) {
                gui_open();
                if (last_flush_result < 0) handheld_ui_error("保存未完成", last_flush_result, UI_ACTION_SAVE);
            }
        }
        else if (command.type == CMD_SAVE) {
            save_requested = IsGameLoaded;
            int result = IsGameLoaded ? flush_backup() : -ENOENT;
            printf("save result=%d (native .sav)\n", result);
        } else if (command.type == CMD_TONE) {
            int result = audio_test_tone();
            if (result == -ECANCELED) puts("tone cancelled");
            else printf("tone result=%d\n", result);
        } else if (command.type == CMD_MENU) gui_open();
        else if (command.type == CMD_VIDEO) {
            int result = 0;
            if (command.frames == 0) video_off();
            else if (command.frames == 1) result = video_init();
            else if (command.frames == 3) result = video_dsi_pattern();
            else result = video_test_pattern();
            printf("lcd result=%d\n", result);
        }
        command_busy = 0;
    }
}

static int enqueue(enum command_type type, const char *path, uint32_t frames)
{
    if (!commands) return -EBUSY;
    struct command command = { .type = type, .frames = frames };
    if (path) {
        if (strlen(path) >= sizeof(command.path)) return -ENAMETOOLONG;
        strcpy(command.path, path);
    }
    taskENTER_CRITICAL();
    if (command_busy) { taskEXIT_CRITICAL(); return -EBUSY; }
    command_busy = 1;
    if (type == CMD_TONE) audio_prepare_test_tone();
    taskEXIT_CRITICAL();
    if (xQueueSend(commands, &command, 0) == pdPASS) return 0;
    command_busy = 0;
    return -EBUSY;
}
int regba_core_start(void)
{
    commands = xQueueCreate(2, sizeof(struct command));
    if (!commands) return -ENOMEM;
    return xTaskCreate(core_task, "ReGBA", CORE_STACK_WORDS, NULL, 3, &core_handle) == pdPASS ? 0 : -ENOMEM;
}
int regba_core_load(const char *path) { return enqueue(CMD_LOAD, path, 0); }
int regba_core_run(uint32_t frames) { return enqueue(CMD_RUN, NULL, frames); }
int regba_core_selftest(void) { return enqueue(CMD_TEST, NULL, 120); }
int regba_core_save(void)
{
    taskENTER_CRITICAL();
    if (running) {
        save_requested = 1;
        taskEXIT_CRITICAL();
        return 0;
    }
    taskEXIT_CRITICAL();
    return enqueue(CMD_SAVE, NULL, 0);
}
int regba_core_menu(void)
{
    taskENTER_CRITICAL();
    if (running) {
        gui_enabled = gui_requested = 1;
        stop_requested = 1;
        taskEXIT_CRITICAL();
        return 0;
    }
    taskEXIT_CRITICAL();
    return enqueue(CMD_MENU, NULL, 0);
}
int regba_core_tone(void) { return enqueue(CMD_TONE, NULL, 0); }
int regba_core_video(unsigned action)
{
    return action <= 3 ? enqueue(CMD_VIDEO, NULL, action) : -EINVAL;
}
void regba_core_stop(void) { stop_requested = 1; audio_request_stop(); }
void regba_core_keys(uint16_t keys) { requested_keys = keys & 0x3FF; }
void regba_core_audio(int enabled)
{
    want_audio = !!enabled;
    if (!enabled) {
        audio_stop_pending = 1;
        audio_request_stop();
    }
}
void regba_core_status(void)
{
    game_stats_t stats;
    game_stats_get(&stats);
    uint16_t board_keys = buttons_get_keys();
    uint16_t serial_keys = requested_keys;
    uint16_t bt_keys = bt_gamepad_get_keys();
    printf("ui enabled=%lu showing=%lu page=%u loaded=%u result=%d path=%s\n",
           (unsigned long)gui_enabled, (unsigned long)gui_showing,
           gui_initialized ? (unsigned)handheld_ui_view()->screen : 0u,
           IsGameLoaded, gui_last_result, IsGameLoaded ? CurrentGamePath : "-");
    uint32_t call_gap, gap_native, gap_pending;
    taskENTER_CRITICAL();
    call_gap = audio_update_max_gap_us;
    gap_native = audio_update_gap_native;
    gap_pending = audio_update_gap_pending;
    taskEXIT_CRITICAL();
    printf("core running=%lu frames=%lu crc=%08lx keys=%03lx audio_requested=%lu serial_keys=%03lx board_keys=%03lx bt_keys=%03lx audio_max_call_gap_us=%lu audio_gap_native=%lu audio_gap_pending=%lu\n",
           (unsigned long)running, (unsigned long)rendered_frames, (unsigned long)frame_crc,
            (unsigned long)(serial_keys | board_keys | bt_keys), (unsigned long)want_audio,
            (unsigned long)serial_keys, (unsigned long)board_keys, (unsigned long)bt_keys,
           (unsigned long)call_gap, (unsigned long)gap_native, (unsigned long)gap_pending);
    if (core_handle)
        printf("core stack min_unused=%lu/%u last_sp=%08lx translation_depth=%lu peak=%lu\n",
               (unsigned long)uxTaskGetStackHighWaterMark(core_handle) * sizeof(StackType_t),
               (unsigned)CORE_STACK_BYTES, (unsigned long)core_last_sp,
               (unsigned long)translation_recursion_level,
               (unsigned long)translation_recursion_peak);
    printf("backup dirty=%u in_flight=%u requested=%lu autosave_idle_ms=2000\n",
           backup_dirty, save_in_flight, (unsigned long)save_requested);
    printf("playback volume=%u frameskip=%u speed=%u emulated=%lu drawn=%lu fast_audio_silent=%u\n",
           audio_get_volume(), playback_frameskip, playback_speed,
           (unsigned long)rendered_frames, (unsigned long)drawn_frames,
           playback_speed != 1);
    printf("game_stats running=%lu valid=%lu cpu_pct=%lu.%lu fps=%lu.%lu window_ms=%lu window_frames=%lu\n",
           (unsigned long)stats.running, (unsigned long)stats.valid,
           (unsigned long)(stats.cpu_tenths / 10u), (unsigned long)(stats.cpu_tenths % 10u),
           (unsigned long)(stats.fps_tenths / 10u), (unsigned long)(stats.fps_tenths % 10u),
           (unsigned long)stats.window_ms, (unsigned long)stats.window_frames);
    if (IsGameLoaded && !strncmp(CurrentGamePath, "/__save_", 8)) {
        uint32_t m[16];
        memcpy(m, ewram_data, sizeof(m));
        printf("save_test magic=%08lx kind=%lu bytes=%lu state=%lu marker=%08lx count=%lu expected_marker=%08lx expected_count=%lu mismatch=%08lx writes=%lu frames=%lu keys=%03lx boot_marker=%08lx boot_count=%lu action=%lu checks=%lu\n",
               (unsigned long)m[0], (unsigned long)m[1], (unsigned long)m[2], (unsigned long)m[3],
               (unsigned long)m[4], (unsigned long)m[5], (unsigned long)m[6], (unsigned long)m[7],
               (unsigned long)m[8], (unsigned long)m[9], (unsigned long)m[10], (unsigned long)m[11],
               (unsigned long)m[12], (unsigned long)m[13], (unsigned long)m[14], (unsigned long)m[15]);
    }
}
