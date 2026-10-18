/* Production mGBA adapter with software host/worker fixtures, no board I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

static unsigned char *fixture_rom;
static const char fixture_rom_path[] = "/GBA/ROMS/probe-1m.gba";
static FILE *fixture_fopen(const char *path, const char *mode)
{
    if (fixture_rom && !strcmp(path, fixture_rom_path) && !strcmp(mode, "rb"))
        return fmemopen(fixture_rom, 1024 * 1024, "rb");
    errno = ENOENT;
    return NULL;
}
static int fixture_stat(const char *path, struct stat *info)
{
    if (!fixture_rom || strcmp(path, fixture_rom_path)) { errno = ENOENT; return -1; }
    memset(info, 0, sizeof(*info));
    info->st_size = 1024 * 1024;
    info->st_mode = S_IFREG | 0444;
    return 0;
}

/* The adapter's file-copy and ownership paths stay real; its input file is RAM. */
#define fopen(path, mode) fixture_fopen(path, mode)
#define stat(path, info) fixture_stat(path, info)
#include "../projects/halley7m-regba/engines/mgba/mgba_adapter.c"
#undef fopen
#undef stat

#ifndef MGBA_TEST_MODE
#define MGBA_TEST_MODE 0
#endif
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %u: %s\n", __LINE__, #expr); return false; } } while (0)

static const emu_backend_ops_t *probe_ops;
static uint64_t probe_now;
static uint16_t probe_keys;
static int probe_stop_choice = -1, probe_end_result, probe_reason, probe_host_error;
static uint32_t probe_stop_count, probe_stop_writes;
static uint64_t probe_pcm_frames;
static uint8_t stored[128 * 1024], pending[128 * 1024];
static size_t stored_size, pending_size;
static char stored_path[512], pending_path[512];
static int worker_busy, worker_result, read_error;
static unsigned queued, completed;
static void *last_read_buffer;

uint64_t platform_time_us(void) { return probe_now; }
size_t platform_free_heap(void) { return 60u * 1024 * 1024; }
int gettimeofday(struct timeval *tv, void *zone)
{
    (void)zone;
    if (tv) { tv->tv_sec = 1767225600; tv->tv_usec = 0; }
    return 0;
}
int storage_prepare_dirs(void) { return 0; }
int battery_save_load(const char *path, void *data, size_t capacity, size_t *size)
{
    if (read_error) return read_error;
    if (!stored_size || strcmp(path, stored_path)) return -ENOENT;
    if (capacity < stored_size) return -EFBIG;
    memcpy(data, stored, stored_size);
    *size = stored_size;
    last_read_buffer = data;
    return 0;
}
int battery_save_submit(const char *path, const void *data, size_t size)
{
    if (worker_busy || size > sizeof(pending)) return -EBUSY;
    if (strncmp(path, MGBA_SAVE_DIR "/", strlen(MGBA_SAVE_DIR) + 1)) return -EINVAL;
    memcpy(pending, data, size);
    pending_size = size;
    snprintf(pending_path, sizeof(pending_path), "%s", path);
    worker_busy = 1;
    worker_result = 0;
    ++queued;
    return 0;
}
int battery_save_busy(void) { return worker_busy; }
int battery_save_result(void) { return worker_result; }
int battery_save_wait(void)
{
    if (worker_busy) {
        memcpy(stored, pending, pending_size);
        stored_size = pending_size;
        strcpy(stored_path, pending_path);
        worker_busy = 0;
        ++completed;
    }
    return worker_result;
}

static uint32_t probe_word(unsigned word)
{ return core->rawRead32(core, 0x02000000 + word * 4, -1); }
static uint16_t probe_input(void *context) { (void)context; return probe_keys; }
static bool probe_render(void *context, uint32_t frame) { (void)context; (void)frame; return true; }
static void probe_video(void *context, const emu_video_frame_t *frame)
{
    (void)context;
    if (frame->width != 240 || frame->height != 160 || frame->stride_pixels != 240 ||
        frame->format != EMU_PIXELS_RGB555) probe_host_error = 1;
}
static size_t probe_audio(void *context, const int16_t *samples, size_t frames, uint32_t rate)
{
    (void)context; (void)samples;
    if (rate != 44100) probe_host_error = 1;
    probe_pcm_frames += frames;
    return frames;
}
static bool probe_frame(void *context, uint32_t frame)
{
    (void)context; (void)frame;
    probe_now += 16742;
    probe_ops->audio_update();
    probe_ops->service_save();
    if (probe_stop_choice >= 0 && probe_word(0) == 0x53565453 && probe_word(3) == 3) {
        probe_ops->diagnostics();
        probe_host_error = 1;
        return true;
    }
    return probe_stop_choice >= 0 && probe_word(0) == 0x53565453 &&
           probe_word(3) == (uint32_t)probe_stop_choice && probe_word(5) == probe_stop_count &&
           probe_word(9) == probe_stop_writes && probe_word(10) >= 1 &&
           probe_word(11) == 0 && probe_word(15) >= 1;
}
static void probe_begin(void *context, uint32_t frames)
{ (void)context; (void)frames; probe_ops->set_audio_enabled(true); }
static void probe_quiesce(void *context)
{ (void)context; probe_ops->set_audio_enabled(false); }
static void probe_end(void *context, int reason)
{ (void)context; probe_reason = reason; probe_end_result = probe_ops->settle_save(); }
static void probe_progress(void *context, unsigned percent) { (void)context; (void)percent; }
static void probe_stack(void *context, const char *where) { (void)context; (void)where; }

static bool wait_guest(unsigned choice, uint32_t count, unsigned writes)
{
    probe_stop_choice = (int)choice;
    probe_stop_count = count;
    probe_stop_writes = writes;
    CHECK(probe_ops->run(1200) == 0);
    CHECK(probe_reason == 1 && probe_end_result == 0 && !probe_host_error);
    CHECK(probe_word(0) == 0x53565453 && probe_word(3) == choice && probe_word(5) == count && probe_word(9) == writes);
    CHECK(probe_word(8) == 0xFFFFFFFF);
    probe_ops->diagnostics();
    probe_stop_choice = -1;
    return true;
}
static bool press_and_wait(uint16_t key, unsigned choice, uint32_t count, unsigned writes)
{
    probe_keys = key;
    CHECK(probe_ops->run(6) == 0); /* Existing board runner holds a key for 100ms. */
    probe_keys = 0;
    return wait_guest(choice, count, writes);
}
static bool check_payload(const uint8_t *image, size_t size, uint32_t marker, uint32_t count)
{
    for (size_t i = 0; i < size; ++i) {
        uint32_t word = (i & 7) < 4 ? marker : count;
        if (image[i] != (uint8_t)(word >> ((i & 3) * 8))) {
            printf("payload mismatch offset=%lu actual=%02x\n", (unsigned long)i, image[i]);
            CHECK(false);
        }
    }
    return true;
}
static bool check_installed(size_t size)
{
    struct GBASavedata *save = &board()->memory.savedata;
    CHECK(save->data == last_read_buffer && save->vf == NULL && save->realVf == NULL);
    CHECK(GBASavedataSize(save) == size && save->currentBank == save->data);
    CHECK(save->command == EEPROM_COMMAND_NULL && save->flashState == FLASH_STATE_RAW);
    CHECK(save->settling == 0 && save->dirty == 0 && save->readBitsRemaining == 0);
    for (size_t i = size; i < SAVE_CAPACITY; ++i) CHECK(save->data[i] == 0xFF);
    return true;
}
static bool native_case(const char *kind, unsigned kind_id, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/__save_%s.gba", kind);
    stored_size = 0;
    CHECK(probe_ops->load(path) == 0);
    CHECK(wait_guest(0, UINT32_MAX, 0));
    CHECK(probe_word(1) == kind_id && probe_word(2) == size);
    CHECK(press_and_wait(1, 1, 1, 1));
    CHECK(probe_ops->request_save() == 0 && probe_ops->flush_save() == 0);
    CHECK(stored_size == size && check_payload(stored, size, 0x53415641, 1));
    uint32_t saved_crc = crc32_bytes(stored, size);
    CHECK(press_and_wait(2, 2, 2, 2));
    CHECK(crc32_bytes(stored, size) == saved_crc); /* Menu-style settle did not write B. */
    emu_backend_state_t before, after;
    probe_ops->get_state(&before);
    CHECK(before.backup_dirty);
    uint32_t frame_before = core->frameCounter(core);
    read_error = -EIO;
    CHECK(probe_ops->reload_save() == -EIO);
    read_error = 0;
    probe_ops->get_state(&after);
    CHECK(after.loaded && after.backup_dirty && !strcmp(before.path, after.path));
    CHECK(core->frameCounter(core) == frame_before && probe_word(3) == 2 && probe_word(5) == 2 && probe_word(9) == 2);
    CHECK(probe_ops->reload_save() == 0 && check_installed(size));
    CHECK(wait_guest(1, 1, 0));
    CHECK(probe_word(12) == 0x53415641 && probe_word(13) == 1);
    CHECK(check_payload(board()->memory.savedata.data, size, 0x53415641, 1));
    CHECK(crc32_bytes(stored, size) == saved_crc);
    CHECK(probe_ops->load(path) == 0 && check_installed(size));
    CHECK(wait_guest(1, 1, 0));
    /* The real 128KiB allocation also supports later native capacity detection. */
    struct GBASavedata *save = &board()->memory.savedata;
    if (kind_id == 3) {
        save->command = EEPROM_COMMAND_WRITE;
        save->writeAddress = 512 * 8;
        GBASavedataWriteEEPROM(save, 1, 65);
        CHECK(GBASavedataSize(save) == 8192);
        for (size_t i = 512; i < 8192; ++i) CHECK(save->data[i] == 0xFF);
    } else if (kind_id == 2) {
        GBASavedataWriteFlash(save, 0x5555, 0xAA);
        GBASavedataWriteFlash(save, 0x2AAA, 0x55);
        GBASavedataWriteFlash(save, 0x5555, 0xB0);
        GBASavedataWriteFlash(save, 0, 1);
        CHECK(GBASavedataSize(save) == 131072 && save->currentBank == save->data + 65536);
        for (size_t i = 65536; i < 131072; ++i) CHECK(save->data[i] == 0xFF);
    }
    probe_ops->unload();
    printf("PASS adapter SAVE %s: A/B/restore A, I/O failure retains B, 128KiB ownership/tail, native reload CRC=%08lx\n",
           kind, (unsigned long)saved_crc);
    return true;
}
static bool mirror_case(void)
{
    stored_size = 0;
    fixture_rom = malloc(1024 * 1024);
    CHECK(fixture_rom);
    memset(fixture_rom, 0xFF, 1024 * 1024);
    memcpy(fixture_rom, selftest_rom, sizeof(selftest_rom));
    uint32_t source_crc = crc32_bytes(fixture_rom, 1024 * 1024);
    CHECK(probe_ops->load(fixture_rom_path) == 0);
    CHECK(board()->memory.romSize == 4 * 1024 * 1024 && board()->memory.rom == (uint32_t*)rom_buffer);
    CHECK(board()->romCrc32 == source_crc);
    for (size_t i = 0; i < 4; ++i) CHECK(!memcmp(rom_buffer + i * 1024 * 1024, fixture_rom, 1024 * 1024));
    CHECK(probe_ops->run(120) == 0 && probe_word(5) == 0x600D);
    probe_ops->unload();
    free(fixture_rom); fixture_rom = NULL;
    printf("PASS adapter 1MiB: four mirrors in one 4MiB ROM buffer, original CRC=%08lx\n", (unsigned long)source_crc);
    return true;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    probe_ops = mgba_adapter_backend();
    const emu_backend_host_t callbacks = { .keys = probe_input, .should_render = probe_render,
        .video = probe_video, .audio_submit = probe_audio, .frame = probe_frame,
        .run_begin = probe_begin, .run_quiesce = probe_quiesce, .run_end = probe_end,
        .load_progress = probe_progress, .stack_diagnostic = probe_stack };
    if (probe_ops->init(&callbacks) < 0) return 1;
    if (MGBA_TEST_MODE == 0 || MGBA_TEST_MODE == 1) {
        int cpu_result = probe_ops->selftest();
        printf("adapter CPU result=%d rendered=%lu native_frames=%lu hardCrash_policy=%u reason=%d end_result=%d\n",
               cpu_result, (unsigned long)rendered_frames, (unsigned long)core->frameCounter(core),
               board()->hardCrash, probe_reason, probe_end_result);
        if (probe_ops->jit_test() != -ENOTSUP || cpu_result < 0 || probe_host_error) return 1;
        puts("PASS adapter CPU: real ops.selftest, callbacks, RGB555/PCM; interpreter has no JIT cache");
        probe_ops->unload();
    }
    if ((MGBA_TEST_MODE == 0 || MGBA_TEST_MODE == 2) && !native_case("sram", 1, 32768)) return 1;
    if ((MGBA_TEST_MODE == 0 || MGBA_TEST_MODE == 3) && !native_case("flash", 2, 65536)) return 1;
    if ((MGBA_TEST_MODE == 0 || MGBA_TEST_MODE == 4) && !native_case("eeprom", 3, 512)) return 1;
    if ((MGBA_TEST_MODE == 0 || MGBA_TEST_MODE == 5) && !mirror_case()) return 1;
    printf("PASS mGBA production adapter diagnostic: queued=%u completed=%u PCM_frames=%llu; no board/FPS claim\n",
           queued, completed, (unsigned long long)probe_pcm_frames);
    return 0;
}
