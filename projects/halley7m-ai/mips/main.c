#include "ai_resources.h"
#include "ai_gui.h"
#include "FreeRTOS.h"
#include "task.h"
#include "platform_runtime.h"
#include "player_display.h"
#include "player_touch.h"
#include "buttons.h"
#include "ai_ethernet.h"
#include "ai_ping.h"
#include "board_entropy.h"
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
    ai_board_entropy_report_t entropy = {0};
    int entropy_result = ai_board_entropy_get_status(&entropy);
    printf("AI_ENTROPY result=%d state=%u error=%d config=%08lx samples=%lu calls=%lu bytes=%lu tls_ready=0\n",
           entropy_result, (unsigned)entropy.state, entropy.fault_error, (unsigned long)entropy.configuration,
           (unsigned long)entropy.hardware_samples, (unsigned long)entropy.read_calls,
           (unsigned long)entropy.bytes_delivered);
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
    unsigned previous_network_state = 0;
    for (;;) {
        uint32_t now = (uint32_t)(platform_time_us() / 1000);
        player_touch_sample_t touch = {0};
        (void)player_touch_poll(now, &touch);
        gui_result = ai_gui_step(now, touch.pressed != 0, touch.x, touch.y, buttons_get_keys());
        ++gui_steps;
        ai_ethernet_snapshot network;
        ai_ethernet_snapshot_get(&network);
        unsigned network_state = network.error ? 1 : !network.started ? 2 : !network.link_up ? 3 :
                                 !network.address_ready ? 4 : !network.dns_ready ? 5 : !network.time_ready ? 6 : 7;
        if (network_state != previous_network_state && !resources.font_result &&
            !resources.dictionary_result && !resources.config_result) {
            static const char *const labels[] = {"", "以太网初始化失败，请查看诊断", "正在启动以太网",
                "网线未连接或正在协商", "网线已连接，正在获取 IP 地址", "已获取 IP，正在解析云端地址",
                "网络地址解析成功，正在校时", "以太网已就绪，云端功能尚未接通"};
            ai_gui_set_status(labels[network_state]);
            previous_network_state = network_state;
        }
        if (gui_result) { printf("AI_GUI error=%d surfaces_retained=1\n", gui_result); vTaskDelete(NULL); }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
static void status(void)
{
    printf("AI_STATUS stage=foundation network=ethernet_bringup tls=unverified air_uart=unwired gui=%d steps=%lu heap=%lu\n",
           gui_result, gui_steps, (unsigned long)platform_free_heap());
    printf("AI_RESOURCES mount=%d font=%d dictionary=%d config=%d config_line=%u key_present=%u\n",
           resources.mount_result, resources.font_result, resources.dictionary_result,
           resources.config_result, resources.config_error_line, resources.config.api_key[0] != 0);
    ai_ethernet_print_status();
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
    printf("AI_NETWORK start=%d\n", ai_ethernet_start());
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
                else if (!strcmp(command, "net phy")) ai_ethernet_print_status();
                else if (!strcmp(command, "net status")) ai_ethernet_print_status();
                else if (!strcmp(command, "net ping")) (void)ai_ping_gateway();
                else if (!strcmp(command, "net ping small")) (void)ai_ping_gateway_small();
                else if (!strcmp(command, "reboot")) platform_reboot();
                else printf("Commands: status, heap, buttons, display, net status, net rng, net phy, net ping, net ping small, reboot\n");
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
