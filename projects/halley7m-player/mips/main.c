#include "player_platform.h"
#include "player_probe.h"
#include "player_config.h"
#include "player_display.h"
#include "player_audio_probe.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void platform_reboot(void) __attribute__((noreturn));

static void console_task(void *unused)
{
    (void)unused;
    char line[512];
    size_t length = 0;
    printf("PLAYER_BOOT core=mips rtos=FreeRTOS entry=80600000 heap_end=83c00000 stage=audio-diagnostics\n");
    printf("PLAYER_NOTE audio_dma=disabled default_volume_percent=%u video_clock=diagnostic-wall\n",
           PLAYER_DEFAULT_VOLUME_PERCENT);
    (void)player_subcore_start();
    printf("player> ");
    for (;;) {
        player_amp_service();
        int c = platform_uart_getchar();
        if (c >= 0) {
            if (c == '\r' || c == '\n') {
                if (!length) continue;
                line[length] = 0;
                printf("\n");
                if (!strcmp(line, "status")) player_amp_print_status();
                else if (!strcmp(line, "reboot")) platform_reboot();
                else if (!strcmp(line, "display")) player_display_print_status();
                else if (!strcmp(line, "display vpg")) printf("DISPLAY_VPG result=%d\n", player_display_test_pattern(true));
                else if (!strcmp(line, "display normal")) printf("DISPLAY_VPG result=%d\n", player_display_test_pattern(false));
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
                } else if (!strcmp(line, "ls") || !strncmp(line, "ls ", 3))
                    player_probe_list(line + (line[2] ? 3 : 2));
                else if (!strncmp(line, "media ", 6)) player_probe_media(line + 6);
                else if (!strcmp(line, "video stop")) player_probe_stop();
                else if (!strncmp(line, "video hold ", 11)) player_probe_video_hold(line + 11);
                else if (!strncmp(line, "video verify ", 13)) player_probe_video_verify(line + 13);
                else if (!strncmp(line, "video ", 6)) {
                    char *path;
                    unsigned long count = strtoul(line + 6, &path, 10);
                    if (path == line + 6 || *path != ' ' || !path[1])
                        printf("Usage: video <frame_limit,0=EOF> <UTF-8 AVI path>\n");
                    else player_probe_video(path + 1, count);
                } else if (!strcmp(line, "heap"))
                    printf("HEAP free=%lu isr_unused=%lu\n", (unsigned long)platform_free_heap(), (unsigned long)platform_isr_stack_unused());
                else printf("Commands: status, heap, reboot, amp start|stop|ping [value], ls [path], media <path>, video <frames> <path>, video stop\n");
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
    configASSERT(xTaskCreate(console_task, "player", 2048, NULL, 3, NULL) == pdPASS);
    vTaskStartScheduler();
    platform_panic("scheduler returned");
}
