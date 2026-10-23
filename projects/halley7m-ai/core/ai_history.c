#include "ai_history.h"
#include <errno.h>
#include <string.h>
#ifndef ESTALE
#define ESTALE 116
#endif
_Static_assert(sizeof(((ai_history *)0)->turns) <= 2u * 1024u * 1024u,
               "display history exceeds its 2 MiB RAM budget");
void ai_history_init(ai_history *h) { memset(h, 0, sizeof(*h)); }
void ai_history_clear(ai_history *h)
{
    uint32_t next = h->next_id;
    memset(h, 0, sizeof(*h));
    h->next_id = next; /* A late event must not match a fresh conversation. */
}
const ai_turn *ai_history_at(const ai_history *h, unsigned index)
{ return index < h->count ? &h->turns[(h->first + index) % AI_HISTORY_ROUNDS] : NULL; }
static ai_turn *last(ai_history *h)
{ return h->count ? &h->turns[(h->first + h->count - 1) % AI_HISTORY_ROUNDS] : NULL; }
static uint32_t next_id(ai_history *h)
{ if (!++h->next_id) ++h->next_id; return h->next_id; }
int ai_history_begin(ai_history *h, const char *user, uint32_t *id)
{
    size_t bytes = strlen(user);
    if (!bytes || bytes > AI_INPUT_BYTES) return -EINVAL;
    if (h->active_id) return -EBUSY;
    if (h->count == AI_HISTORY_ROUNDS) h->first = (h->first + 1) % AI_HISTORY_ROUNDS;
    else ++h->count;
    ai_turn *turn = last(h);
    memset(turn, 0, sizeof(*turn));
    memcpy(turn->user, user, bytes + 1);
    turn->id = h->active_id = next_id(h);
    turn->status = AI_TURN_PENDING;
    *id = turn->id;
    return 0;
}
int ai_history_append(ai_history *h, uint32_t id, const char *text, size_t bytes)
{
    ai_turn *turn = last(h);
    if (!id || id != h->active_id || !turn || turn->id != id) return -ESTALE;
    if (bytes > AI_ANSWER_BYTES - turn->answer_bytes || memchr(text, 0, bytes)) return -E2BIG;
    memcpy(turn->assistant + turn->answer_bytes, text, bytes);
    turn->answer_bytes += bytes;
    turn->assistant[turn->answer_bytes] = 0;
    return 0;
}
int ai_history_finish(ai_history *h, uint32_t id, ai_turn_status status)
{
    ai_turn *turn = last(h);
    if (!id || id != h->active_id || !turn || turn->id != id) return -ESTALE;
    if (status <= AI_TURN_PENDING || status > AI_TURN_LIMIT) return -EINVAL;
    if (status == AI_TURN_COMPLETE && !turn->answer_bytes) return -EINVAL;
    turn->status = status;
    if (status == AI_TURN_COMPLETE) {
        if (h->model_count == AI_CONTEXT_ROUNDS)
            h->model_first = (h->model_first + 1) % AI_CONTEXT_ROUNDS;
        else ++h->model_count;
        unsigned index = (h->model_first + h->model_count - 1) % AI_CONTEXT_ROUNDS;
        h->model_turns[index] = *turn;
    }
    h->active_id = 0;
    return 0;
}
int ai_history_retry(ai_history *h, uint32_t *id)
{
    ai_turn *turn = last(h);
    if (!turn || h->active_id || turn->status == AI_TURN_COMPLETE) return -EINVAL;
    /* Keep the failed/cancelled partial answer visible with its terminal status.
     * Copy before begin, which may advance and reuse a full display ring slot. */
    char user[AI_INPUT_BYTES + 1];
    memcpy(user, turn->user, sizeof(user));
    return ai_history_begin(h, user, id);
}
unsigned ai_history_context(const ai_history *h, const ai_turn *out[AI_CONTEXT_ROUNDS])
{
    for (unsigned i = 0; i < h->model_count; ++i)
        out[i] = &h->model_turns[(h->model_first + i) % AI_CONTEXT_ROUNDS];
    return h->model_count;
}
