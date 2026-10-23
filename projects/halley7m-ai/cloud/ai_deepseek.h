#ifndef AI_DEEPSEEK_H
#define AI_DEEPSEEK_H

#include "ai_history.h"
#include <stddef.h>
#include <stdint.h>

#define AI_DEEPSEEK_HOST "api.deepseek.com"
#define AI_DEEPSEEK_PATH "/chat/completions"
#define AI_DEEPSEEK_MODEL "deepseek-flash"
#define AI_DEEPSEEK_MAX_TOKENS 2048u
#define AI_DEEPSEEK_EVENT_BYTES 65536u
#define AI_DEEPSEEK_TRANSPORT_IMPLEMENTED 0

typedef enum {
    AI_DS_STREAMING, AI_DS_COMPLETE, AI_DS_FAILED, AI_DS_CANCELLED, AI_DS_LIMIT
} ai_deepseek_status;

typedef enum {
    AI_DS_OK = 0, AI_DS_IGNORED = 1,
    AI_DS_ERR_ARGUMENT = -1, AI_DS_ERR_STALE = -2, AI_DS_ERR_WRITE = -3,
    AI_DS_ERR_JSON = -4, AI_DS_ERR_UTF8 = -5, AI_DS_ERR_CAPACITY = -6,
    AI_DS_ERR_SERVER = -7, AI_DS_ERR_MISSING_DONE = -8, AI_DS_ERR_CALLBACK = -9
} ai_deepseek_error;

enum {
    AI_DS_USAGE_PROMPT = 1u, AI_DS_USAGE_COMPLETION = 2u, AI_DS_USAGE_TOTAL = 4u,
    AI_DS_USAGE_CACHE_HIT = 8u, AI_DS_USAGE_CACHE_MISS = 16u,
    AI_DS_USAGE_REQUIRED = 7u
};

typedef struct {
    uint32_t prompt_tokens, completion_tokens, total_tokens;
    uint32_t cache_hit_tokens, cache_miss_tokens;
    unsigned present; /* A missing bit means unknown, not zero. */
} ai_deepseek_usage;

typedef enum { AI_DS_TEXT, AI_DS_FINISHED } ai_deepseek_event_kind;
typedef struct {
    ai_deepseek_event_kind kind;
    uint32_t generation;
    const char *text; /* Valid only for the duration of this callback. */
    size_t text_bytes;
    ai_deepseek_status status;
    ai_deepseek_error error;
    ai_deepseek_usage usage;
} ai_deepseek_event;
/* Return nonzero to reject a TEXT delta and fail the request. FINISHED is a
 * one-shot notification; its return value is ignored. Never reenter the stream. */
typedef int (*ai_deepseek_event_fn)(void *user, const ai_deepseek_event *event);
typedef int (*ai_deepseek_write_fn)(void *user, const char *data, size_t bytes);

/* No allocation, key, socket, HTTP parser, TLS or UI access. Feed only a decoded
 * HTTP 200 text/event-stream body. The caller must serialize access to stream. */
typedef struct {
    uint32_t generation;
    ai_deepseek_status status;
    ai_deepseek_error error;
    ai_deepseek_usage usage;
    size_t answer_bytes, line_bytes, event_bytes;
    unsigned saw_finish, saw_done;
    char finish_reason[48], response_id[257];
    char line[AI_DEEPSEEK_EVENT_BYTES + 1];
    char event[AI_DEEPSEEK_EVENT_BYTES + 1];
    char text[AI_ANSWER_BYTES + 1];
    ai_deepseek_event_fn callback;
    void *user;
} ai_deepseek_stream;

/* Uses the pending history turn whose id equals generation and the last eight
 * completed pairs. With write=NULL, only measures Content-Length. The history
 * must remain unchanged across measure/write; callback failure aborts writing.
 * Write callbacks consume/copy all bytes before returning zero; data is not
 * retained after return. Do not include authentication here. */
int ai_deepseek_write_request(const ai_history *history, uint32_t generation,
                             ai_deepseek_write_fn write, void *user,
                             size_t *written);
int ai_deepseek_stream_init(ai_deepseek_stream *stream, uint32_t generation,
                            ai_deepseek_event_fn callback, void *user);
int ai_deepseek_stream_feed(ai_deepseek_stream *stream, uint32_t generation,
                            const void *data, size_t bytes);
int ai_deepseek_stream_end(ai_deepseek_stream *stream, uint32_t generation);
int ai_deepseek_stream_cancel(ai_deepseek_stream *stream, uint32_t generation);
int ai_deepseek_usage_complete(const ai_deepseek_usage *usage);

#endif
