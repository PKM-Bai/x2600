#include "player_platform.h"
#include "player_probe.h"
#include "player_config.h"
#include "player_display.h"
#include "player_audio_probe.h"
#include "player_audio_client.h"
#include "player_session.h"
#include "player_diagnostics.h"
#include "player_measurement.h"
#include "player_ui_sequence.h"
#include "player_gui.h"
#include "player_storage.h"
#include "buttons.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void platform_reboot(void) __attribute__((noreturn));

static player_session_snapshot_t console_view;
static player_diagnostics_report diagnostics;
static TaskHandle_t gui_handle;
static player_ui_sequence_t ui_sequence;

static void print_ui_sequence(void)
{
    player_ui_sequence_status_t s;
    taskENTER_CRITICAL();
    player_ui_sequence_get_status(&ui_sequence, &s);
    taskEXIT_CRITICAL();
    printf("UI_SEQUENCE active=%lu reason=%lu phase=%lu cycles_started=%lu cycles_completed=%lu elapsed_ms=%lu duration_ms=%lu keys=%03lx aborted=%lu cancel_may_emit_short=%lu\n",
           (unsigned long)s.active, (unsigned long)s.reason, (unsigned long)s.phase,
           (unsigned long)s.cycles_started, (unsigned long)s.cycles_completed,
           (unsigned long)s.elapsed_ms, (unsigned long)s.duration_ms,
           (unsigned long)s.keys, (unsigned long)s.aborted,
           (unsigned long)s.cancel_may_emit_short);
}

static int probes_allowed(void)
{
    player_audio_client_stats audio;
    player_audio_client_get_status(&audio);
#ifndef PLAYER_DIAGNOSTICS_ONLY
    (void)player_session_snapshot(&console_view);
    if (console_view.state != PLAYER_SESSION_IDLE || audio.session_active) {
#else
    if (audio.session_active) {
#endif
        printf("PROBE_BUSY use-stop-before-synchronous-probe\n");
        return 0;
    }
    return 1;
}

static void dump_session_trace(void)
{
    static player_session_trace_t record;
    uint32_t cursor = 0, latest = 0;
    if (!probes_allowed()) return;
    while (player_session_trace_read(cursor, &record, 1, &latest)) {
        player_amp_service();
        printf("SESSION_TRACE seq=%lu session=%lu event=%lu frame=%lu us=%lu samples=%lu duration_us=%lu irq=%lu queues=%08lx flags=%08lx holds=%09llx result=%ld pts_us=%lld frame_duration_us=%lu\n",
               (unsigned long)record.sequence, (unsigned long)record.session,
               (unsigned long)record.event, (unsigned long)record.frame,
               (unsigned long)record.time_us_low, (unsigned long)record.audio_samples,
               (unsigned long)record.duration_us, (unsigned long)record.irq_sequence,
               (unsigned long)record.queues, (unsigned long)record.flags,
               (unsigned long long)record.holds, (long)record.result,
               (long long)record.video_pts_us, (unsigned long)record.video_duration_us);
        cursor = record.sequence;
    }
    printf("SESSION_TRACE_END latest=%lu\n", (unsigned long)latest);
}

static void print_session(void)
{
    (void)player_session_snapshot(&console_view);
    printf("SESSION_STATUS session=%lu state=%u kind=%lu samples=%lu/%lu decoded=%lu displayed=%lu dropped=%lu underruns=%lu volume=%lu pinned=%lu error=%ld request=%lu result=%ld path=%s detail=%s\n",
           (unsigned long)console_view.session_id, (unsigned)console_view.state,
           (unsigned long)console_view.media_kind, (unsigned long)console_view.elapsed_samples,
           (unsigned long)console_view.total_samples, (unsigned long)console_view.decoded_frames,
           (unsigned long)console_view.displayed_frames, (unsigned long)console_view.dropped_frames,
           (unsigned long)console_view.audio_underruns, (unsigned long)console_view.volume_percent,
           (unsigned long)console_view.resources_pinned, (long)console_view.error_code,
           (unsigned long)console_view.last_request_id, (long)console_view.last_request_result,
           console_view.media_path, console_view.error);
    printf("SESSION_QUEUES late=%lu max_late_us=%lu video_packets=%lu audio_packets=%lu pcm_queued=%lu trimmed=%lu audio_padding=%lu weight_approx=%lu\n",
           (unsigned long)console_view.late_frames, (unsigned long)console_view.max_late_us,
           (unsigned long)console_view.video_queue_packets, (unsigned long)console_view.audio_queue_packets,
           (unsigned long)console_view.queued_pcm_samples,
           (unsigned long)console_view.trimmed_frames, (unsigned long)console_view.audio_padding_samples,
           (unsigned long)console_view.weight_approximated_frames);
}

static int session_command(enum player_session_command_kind kind, const char *path, int value)
{
    static uint32_t request_id = 0x80000000u;
    player_session_command_t command = {0};
    command.kind = kind;
    command.request_id = ++request_id;
    command.value = value;
    if (path) {
        if (strlen(path) >= sizeof(command.path)) return -1;
        memcpy(command.path, path, strlen(path) + 1u);
    }
    int result = player_session_post(&command);
    printf("SESSION_POST request=%lu kind=%u result=%d\n",
           (unsigned long)command.request_id, (unsigned)kind, result);
    return result;
}

#ifndef PLAYER_DIAGNOSTICS_ONLY
static void sample_gui_keys(uint32_t now_ms, uint16_t physical_keys)
{
    player_gui_stats_t state;
    player_gui_get_stats(&state);
    taskENTER_CRITICAL();
    uint16_t extra = player_ui_sequence_sample(&ui_sequence, now_ms, physical_keys, &state);
    taskEXIT_CRITICAL();
    player_gui_sample_keys(now_ms, (uint16_t)(physical_keys | extra));
}

static void gui_task(void *unused)
{
    (void)unused;
    int result = player_gui_init(player_gui_display_output());
    printf("GUI_INIT result=%d logical=1280x720 native=720x1280 alpha=straight\n", result);
    if (!result) {
        buttons_set_sample_callback(sample_gui_keys);
        result = buttons_start();
        printf("BUTTONS_INIT result=%d scan_ms=5 debounce_ms=20\n", result);
    }
    if (result) { gui_handle = NULL; vTaskDelete(NULL); return; }
    for (;;) {
        result = player_gui_step((uint32_t)(platform_time_us() / 1000u));
        if (result < 0) {
            buttons_set_sample_callback(NULL);
            printf("GUI_ERROR result=%d surfaces=pinned\n", result);
            gui_handle = NULL;
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
#endif

static void console_task(void *unused)
{
    (void)unused;
    char line[PLAYER_SESSION_PATH_BYTES + 32];
    size_t length = 0;
    printf("PLAYER_BOOT core=mips rtos=FreeRTOS entry=80600000 heap_end=83c00000 stage=player-integration\n");
    printf("PLAYER_NOTE audio_dma=disabled default_volume_percent=%u video_clock=audio-samples\n",
           PLAYER_DEFAULT_VOLUME_PERCENT);
    (void)player_subcore_start();
    player_diagnostics_reset();
#ifndef PLAYER_DIAGNOSTICS_ONLY
    int display_result = player_display_init();
    if (!display_result) display_result = player_display_start();
    printf("DISPLAY_INIT result=%d\n", display_result);
    (void)player_session_init();
    if (!display_result)
        configASSERT(xTaskCreate(gui_task, "player_gui", 4096, NULL, 2, &gui_handle) == pdPASS);
#endif
    printf("player> ");
    for (;;) {
        player_amp_service();
#ifndef PLAYER_DIAGNOSTICS_ONLY
        player_session_step();
#endif
        (void)player_diagnostics_poll(&diagnostics);
#ifndef PLAYER_DIAGNOSTICS_ONLY
        (void)player_measurement_step();
#endif
        int c = platform_uart_getchar();
        if (c >= 0) {
            if (c == '\r' || c == '\n') {
                if (!length) continue;
                line[length] = 0;
                printf("\n");
                if (!strcmp(line, "status")) player_amp_print_status();
                else if (!strcmp(line, "session")) print_session();
                else if (!strcmp(line, "session trace")) dump_session_trace();
                else if (!strcmp(line, "session storage")) {
                    player_storage_seek_stats stats = {0};
                    int result = player_session_storage_stats(&stats);
                    printf("STORAGE_SEEK result=%d enabled=%lu capacity=%lu required=%lu used=%lu build_us=%llu seeks=%lu max_seek_us=%llu\n",
                           result, (unsigned long)stats.enabled,
                           (unsigned long)stats.capacity_entries, (unsigned long)stats.required_entries,
                           (unsigned long)stats.used_entries, (unsigned long long)stats.build_us,
                           (unsigned long)stats.seek_calls, (unsigned long long)stats.max_seek_us);
                }
                else if (!strncmp(line, "play ", 5)) (void)session_command(PLAYER_SESSION_PLAY_PATH, line + 5, 0);
                else if (!strcmp(line, "pause")) (void)session_command(PLAYER_SESSION_PAUSE, NULL, 0);
                else if (!strcmp(line, "resume")) (void)session_command(PLAYER_SESSION_RESUME, NULL, 0);
                else if (!strcmp(line, "stop")) (void)session_command(PLAYER_SESSION_STOP, NULL, 0);
                else if (!strncmp(line, "volume ", 7)) (void)session_command(PLAYER_SESSION_SET_VOLUME, NULL, (int)strtol(line + 7, NULL, 10));
                else if (!strcmp(line, "library")) (void)session_command(PLAYER_SESSION_LIBRARY_OPEN, "0:/", 0);
                else if (!strcmp(line, "metrics")) player_diagnostics_print();
                else if (!strncmp(line, "ui sequence start ", 18)) {
                    player_gui_stats_t state;
                    player_gui_get_stats(&state);
                    (void)player_session_snapshot(&console_view);
                    int result = -1;
                    if (gui_handle && !state.output_error && state.page == PLAYER_GUI_PLAYBACK && !state.menu_open &&
                        console_view.state == PLAYER_SESSION_PLAYING && console_view.volume_percent == PLAYER_DEFAULT_VOLUME_PERCENT) {
                        uint32_t seconds = (uint32_t)strtoul(line + 18, NULL, 10);
                        taskENTER_CRITICAL();
                        result = player_ui_sequence_start(&ui_sequence, (uint32_t)(platform_time_us() / 1000u), seconds);
                        taskEXIT_CRITICAL();
                    }
                    printf("UI_SEQUENCE_START result=%d\n", result);
                }
                else if (!strcmp(line, "ui sequence stop")) {
                    taskENTER_CRITICAL();
                    player_ui_sequence_stop(&ui_sequence);
                    taskEXIT_CRITICAL();
                    print_ui_sequence();
                }
                else if (!strcmp(line, "ui sequence")) print_ui_sequence();
                else if (!strncmp(line, "measure begin ", 14)) {
                    char *label;
                    unsigned long seconds = strtoul(line + 14, &label, 10);
                    while (*label == ' ') ++label;
                    printf("MEASURE_BEGIN result=%d\n", player_measurement_begin(label, (uint32_t)seconds));
                }
                else if (!strcmp(line, "measure stop")) {
                    (void)player_measurement_stop();
                    player_measurement_print_summary();
                }
                else if (!strcmp(line, "measure summary")) player_measurement_print_summary();
                else if (!strcmp(line, "measure dump")) {
                    if (probes_allowed()) printf("MEASURE_DUMP result=%d\n", player_measurement_dump_csv(player_amp_service));
                }
                else if (!strcmp(line, "buttons")) buttons_status();
                else if (!strcmp(line, "tf probe")) {
                    if (probes_allowed()) printf("TF_PROBE_RESULT result=%d\n", player_storage_gpio_probe());
                }
                else if (!strncmp(line, "tf bench ", 9)) {
                    if (probes_allowed()) player_probe_benchmark(line + 9, 0);
                }
                else if (!strcmp(line, "gui")) {
                    player_gui_stats_t s;
                    player_gui_get_stats(&s);
                    printf("GUI_STATUS page=%u menu=%u generation=%lu rendered=%lu presented=%lu dropped_repeats=%lu dropped_actions=%lu error=%d stack_free_words=%lu\n",
                           (unsigned)s.page, s.menu_open, (unsigned long)s.generation,
                           (unsigned long)s.rendered, (unsigned long)s.presented,
                           (unsigned long)s.dropped_repeats, (unsigned long)s.dropped_actions,
                           s.output_error, gui_handle ? (unsigned long)uxTaskGetStackHighWaterMark(gui_handle) : 0);
                }
                else if (!strcmp(line, "reboot")) platform_reboot();
                else if (!strcmp(line, "display")) player_display_print_status();
#ifdef PLAYER_DIAGNOSTICS_ONLY
                else if (!strcmp(line, "display vpg")) printf("DISPLAY_VPG result=%d\n", player_display_test_pattern(true));
                else if (!strcmp(line, "display normal")) printf("DISPLAY_VPG result=%d\n", player_display_test_pattern(false));
#endif
                else if (!strcmp(line, "audio")) player_audio_probe_status();
                else if (!strcmp(line, "tone")) player_audio_probe_tone(96000, 0);
                else if (!strcmp(line, "tone pause")) player_audio_probe_tone(96000, 1);
                else if (!strncmp(line, "tone ", 5)) player_audio_probe_tone((unsigned)strtoul(line + 5, NULL, 10), 0);
                else if (!strcmp(line, "amp start")) (void)player_subcore_start();
                else if (!strcmp(line, "amp stop")) (void)player_subcore_stop();
                else if (!strcmp(line, "amp shared")) (void)player_subcore_shared_test();
                else if (!strncmp(line, "amp ping", 8)) {
                    uint32_t value = (uint32_t)strtoul(line + 8, NULL, 0);
                    printf("AMP_PING result=%d value=%lu\n", player_subcore_ping(value), (unsigned long)value);
                } else if (!strcmp(line, "ls") || !strncmp(line, "ls ", 3)) {
                    if (probes_allowed()) player_probe_list(line + (line[2] ? 3 : 2));
                }
                else if (!strncmp(line, "media ", 6)) {
                    if (probes_allowed()) player_probe_media(line + 6);
                }
                else if (!strncmp(line, "video verify ", 13)) {
                    /* This probe only reads private decoder buffers. It never
                     * takes ownership of the GUI's DPU/rotator route. */
                    if (probes_allowed()) player_probe_video_verify(line + 13);
                }
#ifdef PLAYER_DIAGNOSTICS_ONLY
                else if (!strcmp(line, "video stop")) player_probe_stop();
                else if (!strncmp(line, "video hold ", 11)) player_probe_video_hold(line + 11);
                else if (!strncmp(line, "video ", 6)) {
                    char *path;
                    unsigned long count = strtoul(line + 6, &path, 10);
                    if (path == line + 6 || *path != ' ' || !path[1])
                        printf("Usage: video <frame_limit,0=EOF> <UTF-8 AVI path>\n");
                    else player_probe_video(path + 1, count);
                }
#endif
                else if (!strcmp(line, "heap"))
                    printf("HEAP free=%lu isr_unused=%lu media_stack_free_words=%lu\n",
                           (unsigned long)platform_free_heap(), (unsigned long)platform_isr_stack_unused(),
                           (unsigned long)uxTaskGetStackHighWaterMark(NULL));
                else printf("Commands: play <path>, pause, resume, stop, session, volume <0..100>, library, gui, buttons, metrics, audio, tone, status, heap, reboot, ls [path], media <path>, video verify <path>; display probes require -DiagnosticsOnly\n");
                length = 0;
                printf("player> ");
            } else if ((c == 8 || c == 127) && length) {
                --length;
            } else if (c >= 32 && c != 127 && length + 1 < sizeof(line)) {
                line[length++] = (char)c;
                printf("%c", c);
            }
        }
        vTaskDelay(1);
    }
}

int main(void)
{
    /* FAAD2 fixed-point LC has nested spectral/IMDCT scratch arrays. The
     * measured MIPS .su call chain exceeds 16KiB; reserve 96KiB for this
     * single media/storage owner, then check its board high-water mark. */
    configASSERT(xTaskCreate(console_task, "player", 24576, NULL, 3, NULL) == pdPASS);
    vTaskStartScheduler();
    platform_panic("scheduler returned");
}
