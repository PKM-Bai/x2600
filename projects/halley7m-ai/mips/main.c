#include "ai_resources.h"
#include "ai_gui.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "player_display.h"
#include "player_touch.h"
#include "buttons.h"
#include "board_rng.h"
#include "board_gmac.h"
#include <x2600_hal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static ai_resources resources;
static int gui_result = -ENODEV;
static unsigned long gui_steps;
extern void platform_reboot(void);
static void rng_probe(void)
{
    ai_rng_probe_t report;
    int result = ai_board_rng_probe(256, &report);
    printf("AI_RNG result=%d config=%08lx samples=%lu repeats=%lu zero=%lu ones=%lu one_bits=%lu varying=%08lx elapsed_us=%lu\n",
           result, (unsigned long)report.configuration, (unsigned long)report.samples,
           (unsigned long)report.repeated_neighbors, (unsigned long)report.zero_words,
           (unsigned long)report.all_one_words, (unsigned long)report.one_bits,
           (unsigned long)report.varying_bits, (unsigned long)report.elapsed_us);
}
static void phy_probe(void)
{
    ai_gmac_probe_t report;
    int result = ai_board_gmac_probe(&report);
    printf("AI_PHY result=%d version=%08lx mode=%08lx clock=%08lx ahb0=%lu ahb2=%lu found=%08lx\n",
           result, (unsigned long)report.mac_version, (unsigned long)report.phy_mode,
           (unsigned long)report.mac_clock, (unsigned long)report.ahb0_hz,
           (unsigned long)report.ahb2_hz, (unsigned long)report.found_mask);
    for (unsigned i = 0; i < 32; ++i)
        if (report.found_mask & (UINT32_C(1) << i))
            printf("AI_PHY address=%u id=%04x%04x bmsr=%04x\n", i,
                   report.id1[i], report.id2[i], report.status[i]);
}
static int send_text(void *context, const char *text)
{
    (void)context; (void)text;
    /* No transport is exposed until its TLS and physical-network gates pass. */
    return -ENETDOWN;
}
static void gui_task(void *unused)
{
    (void)unused;
    const char *status = resources.font_result || resources.dictionary_result ?
        "请插入含字体和拼音词库的 TF 卡，然后重启" : resources.config_result ?
        "请在 TF 卡配置 API 密钥，然后重启" : "尚未连接网络 · 输入会保留在本机";
    gui_result = ai_gui_init(player_gui_display_output(), &resources.font, &resources.dictionary,
                             status, send_text, NULL);
    printf("AI_GUI init=%d logical=1280x720\n", gui_result);
    if (gui_result) { vTaskDelete(NULL); return; }
    printf("AI_TOUCH init=%d\n", player_touch_init());
    printf("AI_BUTTONS init=%d\n", buttons_start());
    for (;;) {
        uint32_t now = (uint32_t)(platform_time_us() / 1000);
        player_touch_sample_t touch = {0};
        (void)player_touch_poll(now, &touch);
        gui_result = ai_gui_step(now, touch.pressed != 0, touch.x, touch.y, buttons_get_keys());
        ++gui_steps;
        if (gui_result) { printf("AI_GUI error=%d surfaces_retained=1\n", gui_result); vTaskDelete(NULL); }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
static void status(void)
{
    printf("AI_STATUS stage=foundation network=unverified tls=unverified air_uart=unwired gui=%d steps=%lu heap=%lu\n",
           gui_result, gui_steps, (unsigned long)platform_free_heap());
    printf("AI_RESOURCES mount=%d font=%d dictionary=%d config=%d config_line=%u key_present=%u\n",
           resources.mount_result, resources.font_result, resources.dictionary_result,
           resources.config_result, resources.config_error_line, resources.config.api_key[0] != 0);
}
static void boot_task(void *unused)
{
    (void)unused;
    /* Same reset operation used by the verified player MIPS-only path. */
    LL_GPIO_setPinMode(PE_Instance, 17, GPIO_MODE_OUTPUT0);
    *(volatile uint32_t *)UINT32_C(0xb2200fe0) |= UINT32_C(0x80000000);
    __asm__ volatile("sync" ::: "memory");
    printf("AI_BOOT core=mips rtos=FreeRTOS entry=80600000 heap_end=83c00000 riscv=reset\n");
    ai_resources_load(&resources);
    int display_result = player_display_init();
    if (!display_result) display_result = player_display_start();
    printf("AI_DISPLAY init=%d\n", display_result);
    if (!display_result) configASSERT(xTaskCreate(gui_task, "ai_gui", 8192, NULL, 2, NULL) == pdPASS);
    status();
    char command[40]; size_t length = 0;
    printf("ai> ");
    for (;;) {
        int c = platform_uart_getchar();
        if (c == '\r' || c == '\n') {
            if (length) {
                command[length] = 0;
                if (!strcmp(command, "status") || !strcmp(command, "heap")) status();
                else if (!strcmp(command, "buttons")) buttons_status();
                else if (!strcmp(command, "display")) player_display_print_status();
                else if (!strcmp(command, "net rng")) rng_probe();
                else if (!strcmp(command, "net phy")) phy_probe();
                else if (!strcmp(command, "reboot")) platform_reboot();
                else printf("Commands: status, heap, buttons, display, net rng, net phy, reboot\n");
                length = 0; printf("ai> ");
            }
        } else if ((c == 8 || c == 127) && length) --length;
        else if (c >= 32 && c < 127 && length < sizeof(command) - 1) command[length++] = (char)c;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
int main(void)
{
    configASSERT(xTaskCreate(boot_task, "ai_boot", 4096, NULL, 3, NULL) == pdPASS);
    vTaskStartScheduler();
    platform_panic("AI scheduler returned");
}
