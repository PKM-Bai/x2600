#ifndef PLAYER_PLATFORM_H
#define PLAYER_PLATFORM_H
#include "platform_runtime.h"
#include "player_amp.h"
/* All AMP functions run on the single console/media-control task. */
int player_subcore_start(void);
int player_subcore_stop(void);
int player_subcore_ping(uint32_t value);
int player_subcore_shared_test(void);
/* GPIO-only emergency mute plus remote reset. No DMA/codec reinitialization;
 * buffers remain pinned and a whole-board reset is required for recovery.
 */
void player_subcore_force_silence(void);
void player_amp_service(void);
void player_amp_print_status(void);
const volatile struct player_amp_shared *player_amp_state(void);
#endif
