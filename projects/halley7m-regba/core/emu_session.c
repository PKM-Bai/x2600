/* Common handheld session and task. GPL-2.0-or-later. */
#include "emu_engine.h"
#include "emu_session.h"
#include "platform_runtime.h"
#include "storage.h"
#include "audio.h"
#include "game_stats.h"
#include "video.h"
#include "buttons.h"
#include "bt_gamepad.h"
#include "handheld_ui.h"
#include "playback.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const emu_backend_ops_t *backend;
static QueueHandle_t commands;
/* The recursive translator needs the existing 240KiB stack. No extra task. */
enum { CORE_STACK_WORDS = 60 * 1024, CORE_STACK_BYTES = CORE_STACK_WORDS * sizeof(StackType_t) };
static TaskHandle_t core_handle;
static uintptr_t core_last_sp;
enum command_type { CMD_LOAD, CMD_RUN, CMD_TEST, CMD_SAVE, CMD_TONE, CMD_VIDEO, CMD_MENU };
struct command { enum command_type type; uint32_t frames; char path[EMU_PATH_CAPACITY]; };
static volatile uint32_t running, stop_requested, requested_keys, want_audio;
static volatile uint32_t audio_stop_pending, command_busy, save_requested;
static uint32_t rendered_frames, frame_crc, frame_limit;
static unsigned playback_frameskip, playback_speed = 1;
static volatile uint32_t gui_enabled, gui_showing, gui_requested, gui_input_gate;
static int gui_initialized, gui_storage_ready, gui_last_menu, last_flush_result;
static int gui_last_result;
static uint64_t start_us, next_frame_us;
static bool audio_started;
static void gui_paint(void);
static void gui_progress(unsigned percent);
static void gui_service(void);

static bool backend_loaded(void)
{
    emu_backend_state_t state;
    backend->get_state(&state);
    return state.loaded;
}

/* Used only in the owner task; console status owns its separate snapshot. */
static const char *backend_path(void)
{
    static char path[EMU_PATH_CAPACITY];
    emu_backend_state_t state;
    backend->get_state(&state);
    memcpy(path, state.path, sizeof(path));
    return path;
}

/* Cross-task requests become backend calls only on the emulator task. */
static void forward_save_request(void)
{
    taskENTER_CRITICAL();
    uint32_t requested = save_requested;
    save_requested = 0;
    taskEXIT_CRITICAL();
    if (requested) (void)backend->request_save();
}

static int gamepad_menu_down(void)
{
    bt_gamepad_stats_t pad;
    bt_gamepad_get_stats(&pad);
    return pad.connected && pad.data_valid && !pad.stale && (pad.key2 & BT_GAMEPAD_KEY2_Y);
}

static void host_stack_diagnostic(void *context, const char *where)
{
    (void)context;
    emu_backend_state_t state;
    backend->get_state(&state);
    __asm__ volatile("move %0,$sp" : "=r"(core_last_sp));
    printf("core stack %s frame=%lu sp=%08lx min_unused=%lu/%u translation_depth=%lu peak=%lu\n",
           where, (unsigned long)state.frames, (unsigned long)core_last_sp,
           (unsigned long)uxTaskGetStackHighWaterMark(core_handle) * sizeof(StackType_t),
           (unsigned)CORE_STACK_BYTES, (unsigned long)state.translation_depth,
           (unsigned long)state.translation_peak);
}

static uint16_t host_keys(void *context)
{
    (void)context;
    uint16_t keys = (requested_keys | buttons_get_keys() | bt_gamepad_get_keys()) & 0x3FF;
    if (gui_input_gate) {
        if (!keys) gui_input_gate = 0;
        return 0;
    }
    return keys;
}

static bool host_should_render(void *context, uint32_t frame_index)
{
    (void)context;
    return playback_should_render(frame_index, playback_frameskip) != 0;
}
static void host_video(void *context, const emu_video_frame_t *frame)
{
    (void)context;
    if (frame->format == EMU_PIXELS_RGB555 && frame->width == 240 &&
        frame->height == 160 && frame->stride_pixels >= frame->width)
        video_present_gba(frame->pixels, frame->stride_pixels);
}
static size_t host_audio_submit(void *context, const int16_t *samples,
                              size_t frames, uint32_t sample_rate)
{
    (void)context;
    return audio_submit_stereo(samples, frames, sample_rate);
}

static bool host_frame(void *context, uint32_t completed_frames)
{
    (void)context;
    rendered_frames = completed_frames;
    game_stats_frame(rendered_frames);
    if (stop_requested) return true;
    taskENTER_CRITICAL();
    uint32_t stopped_audio = audio_stop_pending;
    audio_stop_pending = 0;
    taskEXIT_CRITICAL();
    if (stopped_audio) {
        /* An off/on pair may both arrive between frames. The console has
         * already stopped DMA; reconcile the producer before the latest on. */
        audio_stop();
        audio_started = false;
        backend->set_audio_enabled(false);
    }
    int play_audio = want_audio && playback_speed == 1;
    if (play_audio && !audio_started) {
        backend->audio_reset();
        uint64_t before = platform_time_us();
        int result = audio_start();
        if (result == 0) {
            audio_started = true;
            backend->set_audio_enabled(true);
        }
        /* A console off/on during settling cancels this start. Keep the
         * latest request: off is already zero; a later on retries next frame. */
        else if (result != -ECANCELED) want_audio = 0;
        start_us += platform_time_us() - before; /* codec power-up is not guest time */
    } else if (!play_audio && audio_started) {
        audio_stop();
        audio_started = false;
        backend->set_audio_enabled(false);
    }
    (void)backend->audio_update();
    if (gui_enabled) {
        int menu = gamepad_menu_down();
        if (menu && !gui_last_menu) {
            gui_requested = 1;
            stop_requested = 1;
        }
        gui_last_menu = menu;
    }
    forward_save_request();
    if (!gui_requested) backend->service_save();
    if (stop_requested || (frame_limit && rendered_frames >= frame_limit))
        return true;
    /* 280896 GBA cycles/frame at 16777216Hz, preserve the fractional part. */
    next_frame_us = start_us + playback_target_us(rendered_frames, playback_speed);
    while (platform_time_us() < next_frame_us) {
        if (stop_requested) return true;
        vTaskDelay(1);
    }
    return false;
}

static void host_run_begin(void *context, uint32_t frames)
{
    (void)context;
    frame_limit = frames;
    rendered_frames = frame_crc = stop_requested = 0;
    start_us = platform_time_us();
    running = 1;
    game_stats_begin();
}
static void host_run_quiesce(void *context)
{
    (void)context;
    game_stats_end();
    host_stack_diagnostic(NULL, "after-run");
    if (audio_started) audio_stop();
    audio_started = false;
    backend->set_audio_enabled(false);
}
static void host_run_end(void *context, int reason)
{
    (void)context;
    emu_backend_state_t state;
    backend->get_state(&state);
    rendered_frames = state.frames;
    frame_crc = state.frame_crc;
    uint64_t elapsed = platform_time_us() - start_us;
    printf("core stopped reason=%d frames=%lu elapsed_us=%llu RGB555_CRC32=%08lx\n",
           reason, (unsigned long)rendered_frames, (unsigned long long)elapsed, (unsigned long)frame_crc);
    for (;;) {
        forward_save_request();
        last_flush_result = gui_requested ? backend->settle_save() : backend->flush_save();
        /* A save accepted while a snapshot waits still belongs to this ROM.
         * Close that window atomically with publishing the stopped state. */
        taskENTER_CRITICAL();
        if (!save_requested) {
            running = 0;
            taskEXIT_CRITICAL();
            break;
        }
        taskEXIT_CRITICAL();
    }
    if (last_flush_result < 0)
        printf("SAVE stop flush failed result=%d; backup retained in RAM\n", last_flush_result);
}
static void host_load_progress(void *context, unsigned percent)
{
    (void)context;
    if (gui_enabled) gui_progress(percent);
}
static const emu_backend_host_t host = {
    .keys = host_keys, .should_render = host_should_render, .video = host_video,
    .audio_submit = host_audio_submit, .frame = host_frame,
    .run_begin = host_run_begin, .run_quiesce = host_run_quiesce,
    .run_end = host_run_end,
    .load_progress = host_load_progress, .stack_diagnostic = host_stack_diagnostic,
};

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
    backend->run(0);
    gui_showing = 1;
    gui_requested = 0;
    handheld_ui_pause(backend_path());
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
        if (!backend_loaded()) want_audio = 1;
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
    if (backend_loaded()) handheld_ui_pause(backend_path());
    else if (gui_storage_ready) handheld_ui_library(0);
    gui_paint();
}

static void gui_unload_game(void)
{
    backend->unload();
}

static void gui_service(void)
{
    if (!gui_enabled || !gui_showing) return;
    bt_gamepad_stats_t pad;
    bt_gamepad_get_stats(&pad);
    handheld_ui_status(pad.connected, backend_loaded(), want_audio);
    handheld_ui_options(audio_get_volume(), playback_frameskip, playback_speed);
    uint16_t keys = (requested_keys | buttons_get_keys() | pad.keys) & 0x3ffu;
    int menu = pad.connected && pad.data_valid && !pad.stale && (pad.key2 & BT_GAMEPAD_KEY2_Y);
    enum ui_action action = handheld_ui_poll(keys, menu, (uint32_t)(platform_time_us() / 1000u));
    int result = 0;
    if (action == UI_ACTION_NONE) { gui_paint(); return; }
    command_busy = 1;
    if (action == UI_ACTION_RESUME && backend_loaded()) {
        gui_run_game();
        result = last_flush_result;
    } else if (action == UI_ACTION_LAUNCH) {
        const char *path = handheld_ui_launch_path();
        result = backend->check_rom(path);
        if (!result) {
            gui_message("正在保存", "正在完成当前游戏的保存与同步");
            result = backend->flush_save();
        }
        if (!result) {
            gui_message("正在加载", "正在读取游戏  0%\n请稍候");
            result = backend->load(path);
        }
        if (result < 0) {
            handheld_ui_status(pad.connected, backend_loaded(), want_audio);
            handheld_ui_error("无法启动游戏", result, UI_ACTION_LAUNCH);
        } else {
            gui_run_game();
            result = last_flush_result;
        }
    } else if (action == UI_ACTION_SAVE) {
        gui_message("正在保存", "正在将游戏进度写入 TF 卡");
        result = backend->request_save();
        if (!result) result = backend->flush_save();
        if (result < 0) handheld_ui_error("保存未完成", result, UI_ACTION_SAVE);
        else handheld_ui_notice("保存成功", "游戏进度已保存并同步到 TF 卡。", 0, UI_ACTION_NONE);
    } else if (action == UI_ACTION_RELOAD) {
        gui_message("正在读取存档", "读取完成后会重新启动游戏，请在游戏内选择继续");
        result = backend->reload_save();
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
        result = backend->flush_save();
        if (result < 0) handheld_ui_error("保存未完成", result, action);
        else {
            if (action == UI_ACTION_EXIT) gui_unload_game();
            handheld_ui_library(backend_loaded());
        }
    } else if (action == UI_ACTION_SOUND) {
        want_audio = !want_audio;
        handheld_ui_status(pad.connected, backend_loaded(), want_audio);
    } else if (action == UI_ACTION_REFRESH) {
        if (!gui_storage_ready) {
            result = storage_mount();
            if (!result) result = storage_prepare_dirs();
            gui_storage_ready = result == 0;
        }
        if (result < 0) handheld_ui_error("TF 卡未就绪", result, UI_ACTION_REFRESH);
        else handheld_ui_library(backend_loaded());
    }
    gui_last_result = result;
    handheld_ui_options(audio_get_volume(), playback_frameskip, playback_speed);
    command_busy = 0;
    gui_paint();
}

static void core_task(void *arg)
{
    (void)arg;
    if (backend->init(&host) < 0) platform_panic("emulator backend init failed");
    struct command command;
    for (;;) {
        if (xQueueReceive(commands, &command, gui_enabled && gui_showing ? pdMS_TO_TICKS(20) : portMAX_DELAY) != pdTRUE) {
            gui_service();
            continue;
        }
        if (command.type != CMD_MENU && command.type != CMD_SAVE) {
            gui_enabled = gui_showing = 0;
        }
        if (command.type == CMD_LOAD) {
            (void)backend->load(command.path);
        } else if (command.type == CMD_TEST) {
            (void)backend->selftest();
        } else if (command.type == CMD_RUN) {
            backend->run(command.frames);
            if (gui_requested) {
                gui_open();
                if (last_flush_result < 0) handheld_ui_error("保存未完成", last_flush_result, UI_ACTION_SAVE);
            }
        }
        else if (command.type == CMD_SAVE) {
            int result = backend->request_save();
            if (!result) result = backend->flush_save();
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

int emu_session_start(const emu_backend_ops_t *selected)
{
    backend = selected;
    commands = xQueueCreate(2, sizeof(struct command));
    if (!commands) return -ENOMEM;
    return xTaskCreate(core_task, backend->name, CORE_STACK_WORDS, NULL, 3, &core_handle) == pdPASS ? 0 : -ENOMEM;
}
int emu_engine_load(const char *path) { return enqueue(CMD_LOAD, path, 0); }
int emu_engine_run(uint32_t frames) { return enqueue(CMD_RUN, NULL, frames); }
int emu_engine_selftest(void) { return enqueue(CMD_TEST, NULL, 120); }
int emu_engine_save(void)
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
int emu_engine_menu(void)
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
int emu_engine_tone(void) { return enqueue(CMD_TONE, NULL, 0); }
int emu_engine_video(unsigned action)
{
    return action <= 3 ? enqueue(CMD_VIDEO, NULL, action) : -EINVAL;
}
void emu_engine_stop(void) { stop_requested = 1; audio_request_stop(); }
void emu_engine_keys(uint16_t keys) { requested_keys = keys & 0x3FF; }
void emu_engine_audio(int enabled)
{
    want_audio = !!enabled;
    if (!enabled) {
        audio_stop_pending = 1;
        audio_request_stop();
    }
}
void emu_engine_status(void)
{
    if (!backend) return;
    emu_backend_state_t state;
    backend->get_state(&state);
    game_stats_t stats;
    game_stats_get(&stats);
    uint16_t board_keys = buttons_get_keys();
    uint16_t serial_keys = requested_keys;
    uint16_t bt_keys = bt_gamepad_get_keys();
    printf("ui enabled=%lu showing=%lu page=%u loaded=%u result=%d path=%s\n",
           (unsigned long)gui_enabled, (unsigned long)gui_showing,
           gui_initialized ? (unsigned)handheld_ui_view()->screen : 0u,
           state.loaded, gui_last_result, state.loaded ? state.path : "-");
    uint32_t call_gap, gap_native, gap_pending;
    taskENTER_CRITICAL();
    call_gap = state.audio_max_call_gap_us;
    gap_native = state.audio_gap_native;
    gap_pending = state.audio_gap_pending;
    taskEXIT_CRITICAL();
    printf("core running=%lu frames=%lu crc=%08lx keys=%03lx audio_requested=%lu serial_keys=%03lx board_keys=%03lx bt_keys=%03lx audio_max_call_gap_us=%lu audio_gap_native=%lu audio_gap_pending=%lu\n",
           (unsigned long)running, (unsigned long)state.frames, (unsigned long)state.frame_crc,
            (unsigned long)(serial_keys | board_keys | bt_keys), (unsigned long)want_audio,
            (unsigned long)serial_keys, (unsigned long)board_keys, (unsigned long)bt_keys,
           (unsigned long)call_gap, (unsigned long)gap_native, (unsigned long)gap_pending);
    if (core_handle)
        printf("core stack min_unused=%lu/%u last_sp=%08lx translation_depth=%lu peak=%lu\n",
               (unsigned long)uxTaskGetStackHighWaterMark(core_handle) * sizeof(StackType_t),
               (unsigned)CORE_STACK_BYTES, (unsigned long)core_last_sp,
               (unsigned long)state.translation_depth,
               (unsigned long)state.translation_peak);
    printf("backup dirty=%u in_flight=%u requested=%lu autosave_idle_ms=2000\n",
           state.backup_dirty, state.save_in_flight, (unsigned long)(save_requested || state.save_requested));
    printf("playback volume=%u frameskip=%u speed=%u emulated=%lu drawn=%lu fast_audio_silent=%u\n",
           audio_get_volume(), playback_frameskip, playback_speed,
           (unsigned long)state.frames, (unsigned long)state.drawn_frames,
           playback_speed != 1);
    printf("game_stats running=%lu valid=%lu cpu_pct=%lu.%lu fps=%lu.%lu window_ms=%lu window_frames=%lu\n",
           (unsigned long)stats.running, (unsigned long)stats.valid,
           (unsigned long)(stats.cpu_tenths / 10u), (unsigned long)(stats.cpu_tenths % 10u),
           (unsigned long)(stats.fps_tenths / 10u), (unsigned long)(stats.fps_tenths % 10u),
           (unsigned long)stats.window_ms, (unsigned long)stats.window_frames);
    backend->diagnostics();
}

int emu_engine_jit_test(void)
{
    return backend ? backend->jit_test() : -EBUSY;
}
