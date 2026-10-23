#ifndef AI_BOARD_ENTROPY_H
#define AI_BOARD_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* At the measured ~0.94 ms/word cadence, eight words fit the 10 ms budget. */
#define AI_BOARD_ENTROPY_MAX_BYTES 32u

typedef enum {
    AI_ENTROPY_OFF,
    AI_ENTROPY_READY,
    AI_ENTROPY_FAULT_TIMEOUT,
    AI_ENTROPY_FAULT_REPEAT
} ai_board_entropy_state_t;

/* Contains counters and configuration only, never raw random words. READY
 * describes hardware availability, not an estimate of entropy per byte. */
typedef struct {
    ai_board_entropy_state_t state;
    int fault_error;
    uint32_t configuration;
    uint32_t hardware_samples;
    uint32_t read_calls;
    uint32_t bytes_delivered;
} ai_board_entropy_report_t;

/* Call once from a task, after the scheduler starts and before tcpip_init().
 * Uses a single persistent FreeRTOS mutex and two startup words (discarded).
 * READY calls are idempotent; a hardware fault remains latched until reboot.
 * Do not run the standalone board_rng diagnostic concurrently with this owner. */
int ai_board_entropy_init(void);

/* Direct ai_lwip_random_source_t callback; context is unused. Task context only.
 * 1..32 bytes, <=50 ms mutex wait and <=10 ms total hardware wait per call.
 * For a valid output/length, failure clears the entire output. No PRNG/timer
 * fallback is used. Invalid arguments return false without writing. */
bool ai_board_entropy_lwip(void *context, void *data, size_t length);

/* Bounded mutex-protected status snapshot, for diagnostics without raw data. */
int ai_board_entropy_get_status(ai_board_entropy_report_t *report);

/* Signature matches ai_tls_platform_t.entropy, but deliberately returns
 * -ENOSYS with entropy_bits=0 and a cleared output. The documented hardware
 * post-processing block does not establish the required 8 bits/byte. */
int ai_board_entropy_tls(void *context, unsigned char *output, size_t length,
                         size_t *entropy_bits);

#endif
