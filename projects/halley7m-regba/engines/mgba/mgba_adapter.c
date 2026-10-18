/* X2600 mGBA 0.10.5 adapter. GPL-2.0-or-later. */
#include "mgba_adapter.h"
#include "platform_runtime.h"
#include "battery_save.h"
#include "storage.h"
#include "resources.h"
#include "test_resources.h"
#include "FreeRTOS.h"
#include "task.h"
#include <mgba/core/core.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/defines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/crc32.h>
#include <mgba-util/vfs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(COLOR_16_BIT) || defined(COLOR_5_6_5)
#error mGBA adapter requires native RGB555 with COLOR_16_BIT and without COLOR_5_6_5
#endif

enum { SAVE_CAPACITY = 128 * 1024, AUDIO_RATE = 44100, PCM_FRAMES = 256 };
static emu_backend_host_t host;
static struct mCore *core;
static uint16_t screen[240 * 160] __attribute__((aligned(64)));
static uint8_t *rom_buffer;
static char current_path[EMU_PATH_CAPACITY], save_path[EMU_PATH_CAPACITY];
static uint32_t rendered_frames, drawn_frames, frame_crc;
static bool sound_started, backup_dirty, save_in_flight, save_requested;
static uint64_t backup_changed_us, save_attempt_us, audio_update_last_us;
static uint32_t audio_update_max_gap_us, audio_update_gap_native, audio_update_gap_pending;
static int16_t pending_pcm[PCM_FRAMES * 2];
static size_t pcm_frames, pcm_offset;

static void engine_log(struct mLogger *logger, int category, enum mLogLevel level,
                       const char *format, va_list args)
{
    (void)logger;
    if (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) return;
    const char *name = mLogCategoryName(category);
    printf("mGBA %s: ", name ? name : "core");
    vprintf(format, args);
    putchar('\n');
}

static struct mLogger logger = { .log = engine_log, .filter = NULL };

static struct GBA *board(void) { return core->board; }
static bool loaded(void) { return rom_buffer != NULL; }

static uint32_t crc32_bytes(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const uint8_t *builtin_rom(const char *path, size_t *size)
{
#define BUILTIN(name, resource) \
    if (!strcmp(path, name)) { *size = sizeof(resource); return resource; }
    BUILTIN("/__selftest.gba", selftest_rom)
    BUILTIN("/__save_sram.gba", save_sram_rom)
    BUILTIN("/__save_flash.gba", save_flash_rom)
    BUILTIN("/__save_eeprom.gba", save_eeprom_rom)
#undef BUILTIN
    return NULL;
}

static int backup_filename(char *out, size_t capacity, const char *path)
{
    char original[EMU_PATH_CAPACITY + 1];
    int result = regba_backup_path(original, sizeof(original), path);
    if (result < 0) return result;
    int length = snprintf(out, capacity, MGBA_SAVE_DIR "%s", original + strlen(REGBA_SAVE_DIR));
    return length >= 0 && (size_t)length < capacity ? 0 : -ENAMETOOLONG;
}

/* No disk VFile is passed to mGBA: its normal auto-sync cannot write a card
 * save behind the GUI's back. Only this owner task snapshots guest RAM. */
static void observe_backup(void)
{
    if (loaded() && (board()->memory.savedata.dirty & mSAVEDATA_DIRT_NEW)) {
        board()->memory.savedata.dirty &= ~mSAVEDATA_DIRT_NEW;
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
    if (!loaded() || !save_path[0]) return -ENODATA;
    struct GBASavedata *save = &board()->memory.savedata;
    size_t size = GBASavedataSize(save);
    if (!size || !save->data) return -ENODATA;
    int result = battery_save_submit(save_path, save->data, size);
    if (result < 0) return result;
    backup_dirty = save_requested = false;
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
            save_requested = false;
            save_attempt_us = now;
            printf("SAVE request failed result=%d\n", result);
        }
    }
}

static int settle_backup(void)
{
    observe_backup();
    if (!save_in_flight && !battery_save_busy()) return 0;
    int result = battery_save_wait();
    save_in_flight = false;
    if (result < 0) backup_dirty = true;
    return result;
}

static int flush_backup(void)
{
    int result = settle_backup();
    if (result < 0) backup_dirty = true;
    if (!backup_dirty && !save_requested) return 0;
    result = submit_backup();
    if (result < 0) { save_requested = false; return result; }
    result = battery_save_wait();
    save_in_flight = false;
    if (result < 0) backup_dirty = true;
    return result;
}

static void clear_backup_state(void)
{
    backup_dirty = save_in_flight = save_requested = false;
    backup_changed_us = save_attempt_us = 0;
    if (loaded()) board()->memory.savedata.dirty = 0;
}

/* Complete all card reads and size checks before resetting a live guest. */
static int read_backup(const char *path, uint8_t **data, size_t *size)
{
    *data = malloc(SAVE_CAPACITY);
    if (!*data) return -ENOMEM;
    memset(*data, 0xff, SAVE_CAPACITY);
    int result = battery_save_load(path, *data, SAVE_CAPACITY, size);
    if (result < 0) { free(*data); *data = NULL; }
    return result;
}

static enum SavedataType backup_type(size_t size)
{
    enum SavedataType type;
    switch (size) {
    case 512: type = SAVEDATA_EEPROM512; break;
    case 8192: type = SAVEDATA_EEPROM; break;
    case 32768: type = SAVEDATA_SRAM; break;
    case 65536:
        type = board()->memory.savedata.type == SAVEDATA_SRAM512 ?
               SAVEDATA_SRAM512 : SAVEDATA_FLASH512;
        break;
    case 131072: type = SAVEDATA_FLASH1M; break;
    default: return SAVEDATA_FORCE_NONE;
    }
    return type;
}

/* The validated 128KiB staging allocation becomes the cartridge RAM. This
 * pin's mappedMemoryFree() uses free(), so native chip deinit owns it after
 * this point. No second allocation can fail after the guest was reset, and
 * later Flash/EEPROM capacity detection fits the same allocation. */
static void install_backup(uint8_t *data, enum SavedataType type)
{
    struct GBASavedata *save = &board()->memory.savedata;
    GBASavedataDeinit(save);
    GBASavedataInit(save, NULL);
    save->type = type;
    save->data = data;
    /* core->reset preserves the cartridge chip. A cached-ROM reload starts
     * that chip from an idle command/bank state as well as replacing bytes. */
    save->command = EEPROM_COMMAND_NULL;
    save->flashState = FLASH_STATE_RAW;
    save->readBitsRemaining = 0;
    save->readAddress = save->writeAddress = 0;
    save->currentBank = save->data;
    save->settling = save->dirty = 0;
    save->dirtAge = 0;
}

static void reset_audio_update_timing(void)
{
    taskENTER_CRITICAL();
    audio_update_last_us = 0;
    audio_update_max_gap_us = audio_update_gap_native = audio_update_gap_pending = 0;
    taskEXIT_CRITICAL();
}

static void audio_reset(void)
{
    pcm_frames = pcm_offset = 0;
    /* Consume rather than blip_clear(), retaining the fractional rate phase. */
    for (int channel = 0; channel < 2; ++channel) {
        blip_t *blip = core->getAudioChannel(core, channel);
        while (blip_samples_avail(blip) > 0)
            blip_read_samples(blip, pending_pcm, PCM_FRAMES, 0);
    }
}

static void set_audio_enabled(bool enabled)
{
    sound_started = enabled;
    if (enabled) reset_audio_update_timing();
    else pcm_frames = pcm_offset = 0;
}

static int audio_update(void)
{
    if (!loaded()) return 0;
    if (!sound_started) { audio_update_last_us = 0; audio_reset(); return 0; }
    blip_t *left = core->getAudioChannel(core, 0), *right = core->getAudioChannel(core, 1);
    uint64_t now = platform_time_us();
    uint64_t gap = audio_update_last_us ? now - audio_update_last_us : 0;
    audio_update_last_us = now;
    if (gap > audio_update_max_gap_us) {
        taskENTER_CRITICAL();
        audio_update_max_gap_us = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        audio_update_gap_native = (uint32_t)blip_samples_avail(left);
        audio_update_gap_pending = (uint32_t)(pcm_frames - pcm_offset);
        taskEXIT_CRITICAL();
    }
    for (unsigned chunk = 0; chunk < 8; ++chunk) {
        if (pcm_offset < pcm_frames) {
            pcm_offset += host.audio_submit(host.context, pending_pcm + pcm_offset * 2,
                                             pcm_frames - pcm_offset, AUDIO_RATE);
            if (pcm_offset < pcm_frames) return 0;
        }
        int available = blip_samples_avail(left);
        int other = blip_samples_avail(right);
        if (other < available) available = other;
        if (available <= 0) return 0;
        if (available > PCM_FRAMES) available = PCM_FRAMES;
        blip_read_samples(left, pending_pcm, available, 1);
        blip_read_samples(right, pending_pcm + 1, available, 1);
        pcm_frames = (size_t)available;
        pcm_offset = 0;
    }
    return 0;
}

static void unload(void)
{
    if (loaded()) core->unloadROM(core);
    free(rom_buffer);
    rom_buffer = NULL;
    current_path[0] = save_path[0] = 0;
    clear_backup_state();
    pcm_frames = pcm_offset = 0;
}

static int check_rom(const char *path)
{
    if (!path || strlen(path) >= EMU_PATH_CAPACITY) return -ENAMETOOLONG;
    size_t builtin_size;
    if (builtin_rom(path, &builtin_size)) return 0;
    struct stat info;
    if (stat(path, &info) != 0) return -errno;
    if (info.st_size < 0xc0 || (uint64_t)info.st_size > 32u * 1024 * 1024) return -EFBIG;
    FILE *file = fopen(path, "rb");
    if (!file) return -errno;
    uint8_t magic[4];
    size_t amount = fread(magic, 1, sizeof(magic), file);
    int closed = fclose(file);
    if (amount != sizeof(magic) || closed) return -EIO;
    char backup[EMU_PATH_CAPACITY];
    int result = backup_filename(backup, sizeof(backup), path);
    return result < 0 ? result : magic[3] == 0xea ? 0 : -ENOEXEC;
}

static int load(const char *path)
{
    int result = check_rom(path);
    if (result < 0) return result;
    result = flush_backup();
    if (result < 0) {
        printf("load cancelled: unsaved backup result=%d; retry save first\n", result);
        return result;
    }
    char next_save[EMU_PATH_CAPACITY] = {0};
    uint8_t *saved = NULL;
    size_t saved_size = 0;
    if (strcmp(path, "/__selftest.gba")) {
        result = backup_filename(next_save, sizeof(next_save), path);
        if (result < 0) return result;
        result = storage_prepare_dirs();
        if (result < 0) return result;
        result = read_backup(next_save, &saved, &saved_size);
        if (result < 0 && result != -ENOENT) return result;
    }
    size_t file_size = 0;
    const uint8_t *builtin = builtin_rom(path, &file_size);
    FILE *file = NULL;
    if (!builtin) {
        struct stat info;
        if (stat(path, &info)) { free(saved); return -errno; }
        file_size = (size_t)info.st_size;
        file = fopen(path, "rb");
        if (!file) { free(saved); return -errno; }
    }
    /* mGBA allocates another 32MiB for a non-power-of-two image. Preserve its
     * Classic NES 1MiB x4 mirror in our single allocation, before mapping. */
    size_t padded = file_size == 1024u * 1024 ? 4u * 1024 * 1024 : 2u * 1024 * 1024;
    while (padded < file_size) padded *= 2;
    unload();
    if (platform_free_heap() < padded + 2u * 1024 * 1024) result = -ENOMEM;
    else {
        rom_buffer = malloc(padded);
        result = rom_buffer ? 0 : -ENOMEM;
    }
    if (!result) {
        memset(rom_buffer, 0xff, padded);
        if (builtin) memcpy(rom_buffer, builtin, file_size);
        else {
            uint64_t progress_at = 0;
            for (size_t offset = 0; offset < file_size;) {
                size_t amount = file_size - offset;
                if (amount > 65536) amount = 65536;
                if (fread(rom_buffer + offset, 1, amount, file) != amount) { result = -EIO; break; }
                offset += amount;
                uint64_t now = platform_time_us();
                if (offset == file_size || now >= progress_at) {
                    host.load_progress(host.context, (unsigned)(offset * 100u / file_size));
                    progress_at = now + 100000;
                }
            }
        }
    }
    if (file && fclose(file) != 0 && !result) result = -EIO;
    if (!result && file_size == 1024u * 1024) {
        for (size_t copy = 1; copy < 4; ++copy)
            memcpy(rom_buffer + copy * file_size, rom_buffer, file_size);
    }
    if (!result) {
        struct VFile *vf = VFileFromMemory(rom_buffer, padded);
        if (!vf) result = -ENOMEM;
        else if (!core->loadROM(core, vf)) { vf->close(vf); result = -ENOEXEC; }
    }
    if (!result) {
        strcpy(current_path, path);
        strcpy(save_path, next_save);
        /* Padding changes the mapping, not the dump identity used by
         * cartridge overrides during reset. Use mGBA's table-based CRC. */
        if (padded != file_size) board()->romCrc32 = doCrc32(rom_buffer, file_size);
        core->reset(core);
        if (saved) {
            install_backup(saved, backup_type(saved_size));
            saved = NULL;
        }
    }
    free(saved);
    if (result < 0) unload();
    clear_backup_state();
    printf("loaded %s (%lu bytes padded, source_bytes=%lu engine=mGBA)\n",
           path, (unsigned long)padded, (unsigned long)file_size);
    printf("load result=%d errno=%d heap=%lu\n", result ? -1 : 0, result ? -result : 0,
           (unsigned long)platform_free_heap());
    return result;
}

static int reload_save(void)
{
    if (!loaded()) return -ENOENT;
    int result = settle_backup();
    if (result < 0) return result;
    uint8_t *saved;
    size_t size;
    result = read_backup(save_path, &saved, &size);
    if (result < 0) return result;
    enum SavedataType type = backup_type(size);
    if (type == SAVEDATA_FORCE_NONE) { free(saved); return -EILSEQ; }
    /* All I/O and size validation have succeeded. ROM remains mapped. */
    core->reset(core);
    install_backup(saved, type);
    clear_backup_state();
    audio_reset();
    printf("SAVE reloaded cached_rom=1 path=%s\n", current_path);
    return 0;
}

static int run(uint32_t frames)
{
    if (!loaded()) return -ENOENT;
    rendered_frames = drawn_frames = frame_crc = 0;
    audio_reset();
    reset_audio_update_timing();
    host.run_begin(host.context, frames);
    host.stack_diagnostic(host.context, "before-run");
    int result = 0;
    for (;;) {
        bool draw = host.should_render(host.context, rendered_frames);
        board()->video.frameskipCounter = draw ? 0 : 1;
        core->setKeys(core, host.keys(host.context));
        uint32_t before = core->frameCounter(core);
        core->runFrame(core);
        if (core->frameCounter(core) == before) { result = -EFAULT; break; }
        ++rendered_frames;
        if (draw) {
            ++drawn_frames;
            if (drawn_frames == 1 || drawn_frames % 60 == 0)
                frame_crc = crc32_bytes(screen, sizeof(screen));
            const emu_video_frame_t frame = { .pixels = screen, .width = 240,
                .height = 160, .stride_pixels = 240, .format = EMU_PIXELS_RGB555 };
            host.video(host.context, &frame);
        }
        if (host.frame(host.context, rendered_frames) || (frames && rendered_frames >= frames)) break;
    }
    host.run_quiesce(host.context);
    frame_crc = crc32_bytes(screen, sizeof(screen));
    host.run_end(host.context, result ? 2 : 1);
    return result;
}

static int selftest(void)
{
    int result = load("/__selftest.gba");
    if (result < 0) return result;
    result = run(120);
    bool passed = !result && rendered_frames == 120;
    const uint32_t expected[] = {42, 55, 66, 77, 6, 0x600d};
    for (unsigned i = 0; i < 6; ++i) {
        uint32_t value = core->rawRead32(core, 0x02000000u + i * 4, -1);
        printf("mailbox[%u]=%08lx expected=%08lx\n", i, (unsigned long)value, (unsigned long)expected[i]);
        passed &= value == expected[i];
    }
    uint32_t irq = core->rawRead32(core, 0x02000018u, -1);
    for (unsigned i = 0; i < 240 * 160; ++i) passed &= (screen[i] & 0x7fff) == 0x3def;
    passed &= irq > 1;
    printf("VBlank IRQ count=%lu\n", (unsigned long)irq);
    printf("core-test %s (engine=mGBA ARM/Thumb/SWI/IRQ/RAM-code/SMC/frame pixels; interpreter)\n",
           passed ? "PASS" : "FAIL");
    return passed ? 0 : -EIO;
}

static int request_save(void)
{
    if (!loaded()) return -ENOENT;
    save_requested = true;
    return 0;
}

static int init(const emu_backend_host_t *callbacks)
{
    host = *callbacks;
    mLogSetDefaultLogger(&logger);
    core = GBACoreCreate();
    if (!core) return -ENOMEM;
    if (!core->init(core)) { free(core); core = NULL; return -ENOMEM; }
    mCoreInitConfig(core, NULL);
    core->opts.skipBios = true;
    core->opts.useBios = false;
    core->opts.volume = 0x100;
    core->opts.frameskip = 0;
    core->loadConfig(core, &core->config);
    core->setVideoBuffer(core, screen, 240);
    core->setAudioBufferSize(core, 2048);
    for (int channel = 0; channel < 2; ++channel)
        blip_set_rates(core->getAudioChannel(core, channel), core->frequency(core), AUDIO_RATE);
    return 0;
}

static void get_state(emu_backend_state_t *state)
{
    *state = (emu_backend_state_t){ .loaded = loaded(), .frames = rendered_frames,
        .drawn_frames = drawn_frames, .frame_crc = frame_crc, .backup_dirty = backup_dirty,
        .save_in_flight = save_in_flight, .save_requested = save_requested };
    if (loaded()) snprintf(state->path, sizeof(state->path), "%s", current_path);
    taskENTER_CRITICAL();
    state->audio_max_call_gap_us = audio_update_max_gap_us;
    state->audio_gap_native = audio_update_gap_native;
    state->audio_gap_pending = audio_update_gap_pending;
    taskEXIT_CRITICAL();
}

static void diagnostics(void)
{
    if (!loaded() || strncmp(current_path, "/__save_", 8)) return;
    uint32_t m[16];
    for (unsigned i = 0; i < 16; ++i) m[i] = core->rawRead32(core, 0x02000000u + i * 4, -1);
    printf("save_test magic=%08lx kind=%lu bytes=%lu state=%lu marker=%08lx count=%lu expected_marker=%08lx expected_count=%lu mismatch=%08lx writes=%lu frames=%lu keys=%03lx boot_marker=%08lx boot_count=%lu action=%lu checks=%lu\n",
           (unsigned long)m[0], (unsigned long)m[1], (unsigned long)m[2], (unsigned long)m[3],
           (unsigned long)m[4], (unsigned long)m[5], (unsigned long)m[6], (unsigned long)m[7],
           (unsigned long)m[8], (unsigned long)m[9], (unsigned long)m[10], (unsigned long)m[11],
           (unsigned long)m[12], (unsigned long)m[13], (unsigned long)m[14], (unsigned long)m[15]);
}

static int jit_test(void) { return -ENOTSUP; }

const emu_backend_ops_t *mgba_adapter_backend(void)
{
    static const emu_backend_ops_t ops = { .name = "mGBA", .init = init, .load = load,
        .run = run, .selftest = selftest, .unload = unload, .check_rom = check_rom,
        .request_save = request_save, .service_save = service_backup,
        .flush_save = flush_backup, .settle_save = settle_backup, .reload_save = reload_save,
        .audio_reset = audio_reset, .set_audio_enabled = set_audio_enabled,
        .audio_update = audio_update, .get_state = get_state,
        .jit_test = jit_test, .diagnostics = diagnostics };
    return &ops;
}
