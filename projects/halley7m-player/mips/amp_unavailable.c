#include "player_platform.h"
#include <stdio.h>

int player_subcore_start(void)
{
    printf("AMP_UNAVAILABLE reason=riscv-toolchain-and-firmware-missing mips_only=1\n");
    return -1;
}
int player_subcore_stop(void) { return 0; }
int player_subcore_ping(uint32_t value) { (void)value; return -1; }
int player_subcore_shared_test(void)
{
    printf("AMP_SHARED unavailable=1 reason=riscv-toolchain-and-firmware-missing\n");
    return -1;
}
void player_amp_service(void) { }
const volatile struct player_amp_shared *player_amp_state(void) { return NULL; }
void player_amp_print_status(void)
{
    printf("AMP_STATUS active=0 mips_only=1 riscv=missing owner=mips audio_clock_valid=0\n");
}
