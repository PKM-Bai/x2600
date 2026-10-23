#ifndef AI_LWIP_PLATFORM_H
#define AI_LWIP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Install a verified hardware-backed random source before tcpip_init().
 * There is deliberately no fixed-seed/tick/random() fallback. Registration
 * itself does not certify the source; board bring-up must verify DTRNG. */
typedef bool (*ai_lwip_random_source_t)(void *context, void *data, size_t size);
void ai_lwip_set_random_source(ai_lwip_random_source_t source, void *context);
bool ai_lwip_random_ready(void);
uint32_t ai_lwip_random(void);
void ai_lwip_assert(const char *message, const char *file, int line);

/* SNTP UTC is RAM-only. Calling get before a plausible sample returns false.
 * This is clock plumbing, not proof that an unauthenticated NTP peer is trusted. */
void ai_lwip_set_unix_time(uint32_t seconds);
bool ai_lwip_get_unix_time(uint64_t *seconds);
/* Request timestamp for SNTP originate matching. Before synchronization this
 * uses uptime only; it never marks the UTC clock ready. */
void ai_lwip_sntp_time(uint32_t *seconds, uint32_t *microseconds);

#endif
