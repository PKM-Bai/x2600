/* X2600 synchronous ReGBA adapter. GPL-2.0-or-later. */
#include "common.h"
#include "regba_adapter.h"
#include "test_resources.h"
#include "platform_runtime.h"
#include "battery_save.h"
#include "resources.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdarg.h>
#include <setjmp.h>

static emu_backend_host_t host;
static uint16_t screen[240 * 160] __attribute__((aligned(64)));
uint16_t *GBAScreen = screen;
uint32_t GBAScreenPitch = 240;
static volatile uint32_t rendered_frames, frame_crc;
static uint32_t drawn_frames, frame_limit, boundary_pending;
static jmp_buf return_to_task;
static bool jump_valid, sound_started, core_stop_requested;
static int last_run_reason;
static volatile uint32_t save_requested;
static bool backup_dirty, save_in_flight;
static uint64_t backup_changed_us, save_attempt_us;
extern uint32_t backup_update;
extern const uint32_t write_backup_delay;
extern uint32_t save_backup(void);
extern int regba_reload_backup(void);
extern bool IsZippedROM;
extern uint32_t gamepak_ram_pages, gamepak_size;
extern uint32_t translation_recursion_level;
static FILE *mapped_file;
static int16_t pending_pcm[256 * 2];
static size_t pcm_offset, pcm_frames;
static uint64_t audio_update_last_us;
static uint32_t audio_update_max_gap_us, audio_update_gap_native, audio_update_gap_pending;
extern bool IsNintendoBIOS;
extern uint8_t nintendo_bios_sha1[];

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

static int backend_jit_test(void)
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
            pcm_offset += host.audio_submit(host.context, pending_pcm + pcm_offset * 2,
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
    bool draw = host.should_render(host.context, rendered_frames);
    ++rendered_frames;
    if (draw) {
        ++drawn_frames;
        if (drawn_frames == 1 || drawn_frames % 60 == 0)
            frame_crc = crc32_bytes(screen, sizeof(screen));
        const emu_video_frame_t frame = {
            .pixels = screen, .width = 240, .height = 160,
            .stride_pixels = GBAScreenPitch, .format = EMU_PIXELS_RGB555,
        };
        host.video(host.context, &frame);
    }
    boundary_pending = 1;
}
bool ReGBA_IsRenderingNextFrame(void)
{ return host.should_render(host.context, rendered_frames); }
void ReGBA_DisplayFPS(void) { }

/* The native update has committed VCOUNT/IRQ/timer state before this hook.
 * Leave through our own stack only after the session callback returns. */
void regba_frame_boundary(void)
{
    if (!boundary_pending) return;
    boundary_pending = 0;
    if (core_stop_requested || host.frame(host.context, rendered_frames) ||
        (frame_limit && rendered_frames >= frame_limit))
        longjmp(return_to_task, 1);
}

enum ReGBA_Buttons ReGBA_GetPressedButtons(void)
{ return host.keys(host.context) & 0x3ffu; }
uint32_t ReGBA_Menu(enum ReGBA_MenuEntryReason reason)
{ (void)reason; core_stop_requested = true; return 0; }
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
        {
            uint64_t now = platform_time_us();
            if (offset == size || now >= progress_at) {
                host.load_progress(host.context, (unsigned)(offset * 100u / size));
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
void quit(void) { core_stop_requested = true; }
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
    rendered_frames = drawn_frames = frame_crc = boundary_pending = 0;
    core_stop_requested = false;
    pcm_frames = pcm_offset = 0;
    reset_audio_update_timing();
    ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable());
    jump_valid = true;
    host.run_begin(host.context, frames);
    host.stack_diagnostic(host.context, "before-run");
    int reason = setjmp(return_to_task);
    if (reason == 0) {
        reg[CHANGED_PC_STATUS] = 1;
        execute_arm_translate(execute_cycles);
    }
    jump_valid = false;
    last_run_reason = reason;
    host.run_quiesce(host.context);
    frame_crc = crc32_bytes(screen, sizeof(screen));
    host.run_end(host.context, reason);
    bool passed = reason == 1 && (!frames || rendered_frames == frames);
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

static void backend_unload(void)
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

static int backend_check_rom(const char *path)
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

static int backend_init(const emu_backend_host_t *callbacks)
{
    host = *callbacks;
    strcpy(main_path, REGBA_CONFIG_DIR);
    memcpy(bios.rom, openbios, sizeof(openbios));
    init_sound();
    return 0;
}

static int backend_run(uint32_t frames)
{
    if (!IsGameLoaded) return -ENOENT;
    (void)run_game(frames, false);
    return last_run_reason == 1 ? 0 : -EFAULT;
}

static int backend_selftest(void)
{
    int result = core_load_game("/__selftest.gba");
    if (result < 0) return result;
    bool before_pass = run_game(120, true);
    uint32_t before_crc = frame_crc;
    uint32_t before_irq, after_irq;
    memcpy(&before_irq, ewram_data + 24, sizeof(before_irq));
    /* Resume this exact guest state after clearing all translation regions. */
    flush_translation_cache(TRANSLATION_REGION_READONLY, FLUSH_REASON_FULL_CACHE);
    bool after_pass = run_game(120, true);
    memcpy(&after_irq, ewram_data + 24, sizeof(after_irq));
    bool passed = before_pass && after_pass && before_crc == frame_crc && after_irq > before_irq;
    printf("core-retranslate-test %s before=%08lx after=%08lx IRQ=%lu->%lu\n",
           passed ? "PASS" : "FAIL", (unsigned long)before_crc, (unsigned long)frame_crc,
           (unsigned long)before_irq, (unsigned long)after_irq);
    return passed ? 0 : -EIO;
}

static int backend_request_save(void)
{
    if (!IsGameLoaded) return -ENOENT;
    save_requested = 1;
    return 0;
}

static void backend_audio_reset(void)
{
    pcm_frames = pcm_offset = 0;
    ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable());
}

static void backend_set_audio_enabled(bool enabled)
{
    sound_started = enabled;
    if (enabled) reset_audio_update_timing();
    else pcm_frames = pcm_offset = 0;
}

static void backend_get_state(emu_backend_state_t *state)
{
    *state = (emu_backend_state_t){
        .loaded = IsGameLoaded, .frames = rendered_frames,
        .drawn_frames = drawn_frames, .frame_crc = frame_crc,
        .backup_dirty = backup_dirty, .save_in_flight = save_in_flight,
        .save_requested = save_requested != 0,
        .translation_depth = translation_recursion_level,
        .translation_peak = translation_recursion_peak,
    };
    if (IsGameLoaded) snprintf(state->path, sizeof(state->path), "%s", CurrentGamePath);
    taskENTER_CRITICAL();
    state->audio_max_call_gap_us = audio_update_max_gap_us;
    state->audio_gap_native = audio_update_gap_native;
    state->audio_gap_pending = audio_update_gap_pending;
    taskEXIT_CRITICAL();
}

static void backend_diagnostics(void)
{
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

const emu_backend_ops_t *regba_adapter_backend(void)
{
    static const emu_backend_ops_t ops = {
        .name = "ReGBA", .init = backend_init, .load = core_load_game,
        .run = backend_run, .selftest = backend_selftest,
        .unload = backend_unload, .check_rom = backend_check_rom,
        .request_save = backend_request_save, .service_save = service_backup,
        .flush_save = flush_backup, .settle_save = settle_backup,
        .reload_save = core_reload_save, .audio_reset = backend_audio_reset,
        .set_audio_enabled = backend_set_audio_enabled, .audio_update = ReGBA_AudioUpdate,
        .get_state = backend_get_state,
        .jit_test = backend_jit_test, .diagnostics = backend_diagnostics,
    };
    return &ops;
}
