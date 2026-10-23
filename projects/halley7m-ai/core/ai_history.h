#ifndef AI_HISTORY_H
#define AI_HISTORY_H
#include <stddef.h>
#include <stdint.h>
#define AI_HISTORY_ROUNDS 50u
#define AI_CONTEXT_ROUNDS 8u
#define AI_INPUT_BYTES 4096u
#define AI_ANSWER_BYTES 32768u
typedef enum { AI_TURN_PENDING, AI_TURN_COMPLETE, AI_TURN_FAILED, AI_TURN_CANCELLED, AI_TURN_LIMIT } ai_turn_status;
typedef struct {
    uint32_t id;
    ai_turn_status status;
    size_t answer_bytes;
    char user[AI_INPUT_BYTES + 1], assistant[AI_ANSWER_BYTES + 1];
} ai_turn;
typedef struct {
    /* Display storage stays within its 2 MiB budget. Completed model history
     * is a separate copy so failed/cancelled display turns cannot evict it.
     * The eight copies require 288 KiB of text plus metadata/padding (actual
     * sizeof(model_turns)); charge this to the 8 MiB protocol/network budget. */
    ai_turn turns[AI_HISTORY_ROUNDS];
    ai_turn model_turns[AI_CONTEXT_ROUNDS];
    unsigned first, count;
    unsigned model_first, model_count;
    uint32_t next_id, active_id;
} ai_history;
void ai_history_init(ai_history *history);
void ai_history_clear(ai_history *history);
int ai_history_begin(ai_history *history, const char *user, uint32_t *id);
int ai_history_append(ai_history *history, uint32_t id, const char *text, size_t bytes);
int ai_history_finish(ai_history *history, uint32_t id, ai_turn_status status);
int ai_history_retry(ai_history *history, uint32_t *id);
const ai_turn *ai_history_at(const ai_history *history, unsigned index);
unsigned ai_history_context(const ai_history *history, const ai_turn *out[AI_CONTEXT_ROUNDS]);
#endif
