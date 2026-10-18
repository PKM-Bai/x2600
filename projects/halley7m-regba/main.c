#include "platform_runtime.h"
#include "runtime_selftest.h"
#include "FreeRTOS.h"
#include "task.h"
#include "storage.h"
#include "audio.h"
#include "video.h"
#include "buttons.h"
#include "bt_gamepad.h"
#include "battery_save.h"
#include "regba_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

static void help(void)
{
    puts("help | status | cpu [seconds,1-300,default10] | ram-test | rtos-test | jit-test | core-test\n"
         "mount | ls [path] | prepare | crc <path> | rwtest\n"
         "tf-probe (unmounted GPIO CMD0/CMD8, 8 trials)\n"
         "load <path> | run <frames,0=continuous> | stop | menu | keys <hex> | buttons | btpad\n"
         "audio on|off|silent | tone (2s, soft 500Hz) | save (also while running)\n"
         "lcd on|off|test|vpg|diag\n"
         "Boots to the game library. Y opens the in-game menu. No NOR writes.");
}

static void status(void)
{
    audio_stats_t a;
    audio_get_stats(&a);
    printf("time_us=%llu tick=%lu heap_free=%lu tasks=%lu console_stack_free=%lu bytes\n",
           (unsigned long long)platform_time_us(), (unsigned long)xTaskGetTickCount(),
           (unsigned long)platform_free_heap(), (unsigned long)uxTaskGetNumberOfTasks(),
           (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    regba_core_status();
    buttons_status();
    bt_gamepad_status();
    battery_save_status();
    video_status();
    printf("audio running=%lu muted=%lu rate=%lu queued=%lu in=%lu out=%lu DMA_done=%lu queue_under=%lu FIFO_under=%lu errors=%lu timeouts=%lu nonzero=%lu peak=%lu volume=%u\n",
           (unsigned long)a.running, (unsigned long)a.muted,
           (unsigned long)a.input_rate, (unsigned long)a.queued_frames,
           (unsigned long)a.input_frames, (unsigned long)a.output_frames,
           (unsigned long)a.completed_periods, (unsigned long)a.queue_underruns,
           (unsigned long)a.fifo_underruns, (unsigned long)a.dma_errors, (unsigned long)a.dma_timeouts,
           (unsigned long)a.input_nonzero_frames, (unsigned long)a.input_peak, audio_get_volume());
    printf("audio_timing max_ready_gap_us=%lu underrun_ready_gap_us=%lu max_dma_restart_us=%lu first_queue_underrun_us=%llu last_ready_periods=%lu last_ready_active=%lu last_ready_dma_age_us=%lu first_under_ready_periods=%lu first_under_active=%lu first_under_dma_age_us=%lu first_fifo_underrun_us=%llu first_fifo_completed_periods=%lu\n",
           (unsigned long)a.max_ready_gap_us, (unsigned long)a.underrun_ready_gap_us,
           (unsigned long)a.max_dma_restart_us,
           (unsigned long long)a.first_queue_underrun_us,
           (unsigned long)a.last_ready_periods, (unsigned long)a.last_ready_active,
           (unsigned long)a.last_ready_dma_age_us,
           (unsigned long)a.first_under_ready_periods, (unsigned long)a.first_under_active,
           (unsigned long)a.first_under_dma_age_us,
           (unsigned long long)a.first_fifo_underrun_us,
           (unsigned long)a.first_fifo_completed_periods);
}

static int cpu_stats(unsigned seconds)
{
    enum { TASK_CAPACITY = 16 };
    TaskStatus_t before[TASK_CAPACITY], after[TASK_CAPACITY];
    uint64_t begin, end, elapsed, accounted = 0, idle_us = 0;
    uint64_t deltas[TASK_CAPACITY];
    TaskHandle_t idle = xTaskGetIdleTaskHandle();

    printf("cpu sampling seconds=%u\n", seconds);
    /* Settle this console task's preceding output before the first snapshot.
     * Snapshot collection suspends scheduling, so 64-bit task counters cannot
     * be modified by a task switch while uxTaskGetSystemState reads them. */
    vTaskDelay(1);
    UBaseType_t count_before = uxTaskGetSystemState(before, TASK_CAPACITY, &begin);
    if (!count_before)
        return -ENOMEM;
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000u));
    UBaseType_t count_after = uxTaskGetSystemState(after, TASK_CAPACITY, &end);
    if (!count_after || count_before != count_after)
        return -EAGAIN;
    elapsed = end - begin;
    if (!elapsed)
        return -EAGAIN;

    for (UBaseType_t i = 0; i < count_after; ++i) {
        UBaseType_t j;
        for (j = 0; j < count_before; ++j)
            if (before[j].xHandle == after[i].xHandle &&
                before[j].xTaskNumber == after[i].xTaskNumber)
                break;
        /* Report a stable task set rather than mixing newly created/deleted
         * tasks into a window whose starting counters are unavailable. */
        if (j == count_before || after[i].ulRunTimeCounter < before[j].ulRunTimeCounter)
            return -EAGAIN;
        deltas[i] = after[i].ulRunTimeCounter - before[j].ulRunTimeCounter;
        accounted += deltas[i];
        if (after[i].xHandle == idle)
            idle_us = deltas[i];
    }
    uint64_t idle_pct = idle_us * 10000u / elapsed;
    if (idle_pct > 10000u)
        return -EAGAIN;
    uint64_t busy_pct = 10000u - idle_pct;
    uint64_t accounted_pct = accounted * 10000u / elapsed;
    printf("cpu window_us=%llu busy_pct=%lu.%02lu idle_pct=%lu.%02lu accounted_pct=%lu.%02lu tasks=%lu\n",
           (unsigned long long)elapsed,
           (unsigned long)(busy_pct / 100u), (unsigned long)(busy_pct % 100u),
           (unsigned long)(idle_pct / 100u), (unsigned long)(idle_pct % 100u),
           (unsigned long)(accounted_pct / 100u), (unsigned long)(accounted_pct % 100u),
           (unsigned long)count_after);
    for (UBaseType_t i = 0; i < count_after; ++i) {
        uint64_t percent = deltas[i] * 10000u / elapsed;
        printf("cpu_task name=%s priority=%lu runtime_us=%llu cpu_pct=%lu.%02lu stack_free_bytes=%lu\n",
               after[i].pcTaskName, (unsigned long)after[i].uxCurrentPriority,
               (unsigned long long)deltas[i],
               (unsigned long)(percent / 100u), (unsigned long)(percent % 100u),
               (unsigned long)after[i].usStackHighWaterMark * sizeof(StackType_t));
    }
    puts("cpu done");
    return 0;
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') ++arg; }
    else arg = line + strlen(line);
    int result = 0;
    if (!strcmp(line, "help")) help();
    else if (!strcmp(line, "status")) status();
    else if (!strcmp(line, "buttons")) buttons_status();
    else if (!strcmp(line, "menu") && !*arg) result = regba_core_menu();
    else if (!strcmp(line, "btpad") && !*arg) bt_gamepad_status();
    else if (!strcmp(line, "cpu")) {
        char *end;
        unsigned long seconds = *arg ? strtoul(arg, &end, 10) : 10;
        if ((*arg && *end) || seconds < 1 || seconds > 300) result = -EINVAL;
        else result = cpu_stats((unsigned)seconds);
    }
    else if (!strcmp(line, "ram-test")) result = runtime_memory_test();
    else if (!strcmp(line, "rtos-test")) result = runtime_selftest_start();
    else if (!strcmp(line, "jit-test")) result = regba_jit_test();
    else if (!strcmp(line, "core-test")) result = regba_core_selftest();
    else if (!strcmp(line, "lcd") && !strcmp(arg, "diag")) video_diagnostics();
    else if (!strcmp(line, "lcd") && (!strcmp(arg, "on") ||
             !strcmp(arg, "off") || !strcmp(arg, "test") || !strcmp(arg, "vpg")))
        result = regba_core_video(!strcmp(arg, "off") ? 0 : !strcmp(arg, "on") ? 1 :
                                 !strcmp(arg, "vpg") ? 3 : 2);
    else if (!strcmp(line, "mount")) result = storage_mount();
    else if (!strcmp(line, "tf-probe") && !*arg) result = storage_tf_probe();
    else if (!strcmp(line, "prepare")) result = storage_prepare_dirs();
    else if (!strcmp(line, "ls")) result = storage_list(*arg ? arg : "/GBA/ROMS");
    else if (!strcmp(line, "crc") && *arg) {
        uint32_t crc, bytes;
        result = storage_crc32(arg, &crc, &bytes);
        if (!result) printf("CRC32=%08lx bytes=%lu %s\n", (unsigned long)crc, (unsigned long)bytes, arg);
    } else if (!strcmp(line, "rwtest")) result = storage_write_test();
    else if (!strcmp(line, "load") && *arg) result = regba_core_load(arg);
    else if (!strcmp(line, "run") && *arg) {
        char *end;
        unsigned long frames = strtoul(arg, &end, 10);
        if (*end) result = -1; else result = regba_core_run(frames);
    } else if (!strcmp(line, "stop")) regba_core_stop();
    else if (!strcmp(line, "keys") && *arg) {
        char *end;
        unsigned long keys = strtoul(arg, &end, 16);
        if (*end || keys > 0x3FF) result = -1; else regba_core_keys(keys);
    } else if (!strcmp(line, "audio") && (!strcmp(arg, "on") ||
               !strcmp(arg, "off") || !strcmp(arg, "silent"))) {
        if (strcmp(arg, "off")) audio_set_muted(!strcmp(arg, "silent"));
        regba_core_audio(strcmp(arg, "off") != 0);
    }
    else if (!strcmp(line, "tone")) result = regba_core_tone();
    else if (!strcmp(line, "save")) result = regba_core_save();
    else { puts("Unknown command/argument; type help"); return; }
    if (result) printf("result=%d\n", result);
}

static void console_task(void *arg)
{
    (void)arg;
    int result = storage_start();
    printf("filesystem task=%d\n", result);
    result = battery_save_start();
    printf("save task=%d (native .sav, 2s idle autosave)\n", result);
    result = regba_core_start();
    printf("ReGBA task=%d\n", result);
    result = buttons_start();
    printf("buttons task=%d (SW6=Start PE13, SW3=A PE14, SW4=B PE15)\n", result);
    result = bt_gamepad_init();
    printf("btpad init=%d (UART2 RX=PD12, J10 pin 2, 115200 8N1)\n", result);
    help();
    result = regba_core_menu();
    printf("game library start=%d\n", result);
    char line[544];
    size_t length = 0;
    bool overflow = false;
    bool last_cr = false;
    printf("x2600> ");
    for (;;) {
        int ch = platform_uart_getchar();
        if (ch < 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (ch == '\n' && last_cr) { last_cr = false; continue; }
        last_cr = ch == '\r';
        if (ch == '\r' || ch == '\n') {
            putchar('\n'); line[length] = 0;
            if (overflow) puts("Line too long; command discarded");
            else if (length) command(line);
            length = 0; overflow = false; printf("x2600> ");
        } else if (ch == 8 || ch == 127) {
            if (length) {
                do { --length; }
                while (length && ((unsigned char)line[length] & 0xc0u) == 0x80u);
                printf("\b \b");
            }
        } else if (ch >= 32) {
            if (length + 1 < sizeof(line) && !overflow) { line[length++] = ch; putchar(ch); }
            else overflow = true;
        }
    }
}

int main(void)
{
    platform_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("X2600M ReGBA stage0-5 bring-up; 64MiB DDR; J9 SSI0 TF; portrait MIPI; UART0 115200");
    /* UART TX polls at 115200 baud. Share the core's priority so a long
     * status line cannot exclude the PCM producer for an entire audio ring.
     */
    if (xTaskCreate(console_task, "console", 4096, NULL, 3, NULL) != pdPASS)
        platform_panic("console allocation");
    vTaskStartScheduler();
    platform_panic("scheduler returned");
}
