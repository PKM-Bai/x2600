#ifndef PLAYER_DISPLAY_HOST_CACHE_H
#define PLAYER_DISPLAY_HOST_CACHE_H
#include <stdint.h>
void fake_cache_publish(unsigned long *pointer, long bytes);
static inline void CleanDCache_by_Addr(unsigned long *pointer, long bytes)
{ fake_cache_publish(pointer,bytes); }
static inline void CleanInvalidateDCache_by_Addr(unsigned long *pointer, long bytes)
{ fake_cache_publish(pointer,bytes); }
#endif
