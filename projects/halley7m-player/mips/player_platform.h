#ifndef PLAYER_PLATFORM_H
#define PLAYER_PLATFORM_H
#include "platform_runtime.h"
#include "player_amp.h"
/* All AMP functions run on the single console/media-control task. */
int player_subcore_start(void);
int player_subcore_stop(void);
int player_subcore_ping(uint32_t value);
int player_subcore_shared_test(void);
void player_amp_service(void);
void player_amp_print_status(void);
const volatile struct player_amp_shared *player_amp_state(void);
#endif
