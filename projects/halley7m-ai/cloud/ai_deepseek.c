#include "ai_deepseek.h"
#include <limits.h>
#include <string.h>
#define JSMN_STATIC
#define JSMN_STRICT
#define JSMN_PARENT_LINKS
#include "vendor/jsmn/jsmn.h"

/* JSON token workspace is bounded at 5 KiB on the parsing task's stack. */
#define JSON_TOKENS 256u

static int utf8_codepoint(const unsigned char *s, size_t n, uint32_t *cp,
                          size_t *used)
{
    unsigned more;
    uint32_t value, minimum;
    if (!n) return AI_DS_ERR_UTF8;
    if (s[0] < 0x80) { *cp = s[0]; *used = 1; return AI_DS_OK; }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) { more = 1; value = s[0] & 31u; minimum = 0x80; }
    else if (s[0] >= 0xe0 && s[0] <= 0xef) { more = 2; value = s[0] & 15u; minimum = 0x800; }
    else if (s[0] >= 0xf0 && s[0] <= 0xf4) { more = 3; value = s[0] & 7u; minimum = 0x10000; }
    else return AI_DS_ERR_UTF8;
    if (n < more + 1u) return AI_DS_ERR_UTF8;
    for (unsigned i = 1; i <= more; ++i) {
        if ((s[i] & 0xc0u) != 0x80u) return AI_DS_ERR_UTF8;
        value = (value << 6) | (s[i] & 63u);
    }
    if (value < minimum || value > 0x10ffffu || (value >= 0xd800 && value <= 0xdfff))
        return AI_DS_ERR_UTF8;
    *cp = value; *used = more + 1u;
    return AI_DS_OK;
}

static int validate_utf8(const char *s, size_t n)
{
    size_t offset = 0, used;
    uint32_t cp;
    while (offset < n) {
        if (utf8_codepoint((const unsigned char *)s + offset, n - offset, &cp, &used))
            return AI_DS_ERR_UTF8;
        if (!cp) return AI_DS_ERR_UTF8;
        offset += used;
    }
    return AI_DS_OK;
}

typedef struct { ai_deepseek_write_fn write; void *user; size_t bytes; } writer;
static int put(writer *w, const char *s, size_t n)
{
    if (n > SIZE_MAX - w->bytes) return AI_DS_ERR_CAPACITY;
    if (w->write && w->write(w->user, s, n)) return AI_DS_ERR_WRITE;
    w->bytes += n;
    return AI_DS_OK;
}
static int literal(writer *w, const char *s) { return put(w, s, strlen(s)); }
static int string_json(writer *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(s), run = 0;
    int rc = validate_utf8(s, n);
    if (rc || (rc = literal(w, "\""))) return rc;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c != '"' && c != '\\') continue;
        if ((rc = put(w, s + run, i - run))) return rc;
        if (c == '"' || c == '\\') {
            char escaped[2] = {'\\', (char)c};
            rc = put(w, escaped, sizeof(escaped));
        } else {
            char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15u]};
            rc = put(w, escaped, sizeof(escaped));
        }
        if (rc) return rc;
        run = i + 1;
    }
    if ((rc = put(w, s + run, n - run))) return rc;
    return literal(w, "\"");
}
static int message(writer *w, const char *role, const char *text)
{
    int rc;
    if ((rc = literal(w, ",{\"role\":\"")) || (rc = literal(w, role)) ||
        (rc = literal(w, "\",\"content\":")) || (rc = string_json(w, text))) return rc;
    return literal(w, "}");
}

int ai_deepseek_write_request(const ai_history *h, uint32_t generation,
                             ai_deepseek_write_fn write, void *user, size_t *written)
{
    const ai_turn *context[AI_CONTEXT_ROUNDS], *current;
    writer w = {write, user, 0};
    unsigned count;
    int rc;
    if (written) *written = 0;
    if (!h || !written || !generation) return AI_DS_ERR_ARGUMENT;
    current = h->count ? ai_history_at(h, h->count - 1) : NULL;
    if (!current || h->active_id != generation || current->id != generation ||
        current->status != AI_TURN_PENDING) return AI_DS_ERR_STALE;
    count = ai_history_context(h, context);
    rc = literal(&w, "{\"model\":\"" AI_DEEPSEEK_MODEL "\",\"thinking\":{\"type\":\"disabled\"},"
                     "\"max_tokens\":2048,\"stream\":true,\"stream_options\":{\"include_usage\":true},"
                     "\"messages\":[{\"role\":\"system\",\"content\":");
    if (rc) goto out;
    rc = string_json(&w, "你是中文问答助手。默认使用简体中文和纯文本，回答简明准确。");
    if (rc || (rc = literal(&w, "}"))) goto out;
    for (unsigned i = 0; i < count; ++i) {
        if ((rc = message(&w, "user", context[i]->user)) ||
            (rc = message(&w, "assistant", context[i]->assistant))) goto out;
    }
    if (!(rc = message(&w, "user", current->user))) rc = literal(&w, "]}");
out:
    *written = w.bytes;
    return rc;
}

static int notify(ai_deepseek_stream *s, ai_deepseek_event_kind kind,
                  const char *text, size_t bytes)
{
    ai_deepseek_event event;
    if (!s->callback) return 0;
    event.kind = kind; event.generation = s->generation;
    event.text = text; event.text_bytes = bytes; event.status = s->status;
    event.error = s->error; event.usage = s->usage;
    return s->callback(s->user, &event);
}
static int fail(ai_deepseek_stream *s, int error)
{
    s->error = (ai_deepseek_error)error;
    s->status = error == AI_DS_ERR_CAPACITY ? AI_DS_LIMIT : AI_DS_FAILED;
    (void)notify(s, AI_DS_FINISHED, NULL, 0);
    return error;
}
int ai_deepseek_usage_complete(const ai_deepseek_usage *u)
{ return u && (u->present & AI_DS_USAGE_REQUIRED) == AI_DS_USAGE_REQUIRED; }

int ai_deepseek_stream_init(ai_deepseek_stream *s, uint32_t generation,
                            ai_deepseek_event_fn callback, void *user)
{
    if (!s || !generation) return AI_DS_ERR_ARGUMENT;
    memset(s, 0, sizeof(*s));
    s->generation = generation; s->callback = callback; s->user = user;
    return AI_DS_OK;
}

static int token_is(const char *json, const jsmntok_t *t, const char *value)
{
    size_t n = strlen(value);
    return t->end >= t->start && (size_t)(t->end - t->start) == n &&
           !memcmp(json + t->start, value, n);
}
static int token_null(const char *json, const jsmntok_t *t)
{ return t->type == JSMN_PRIMITIVE && token_is(json, t, "null"); }
/* Return value index, -1 for absent, -2 for malformed/duplicate fields. */
static int field(const char *json, const jsmntok_t *tokens, int count,
                 int object, const char *key)
{
    int found = -1;
    if (object < 0 || object >= count || tokens[object].type != JSMN_OBJECT) return -2;
    for (int i = object + 1; i < count && tokens[i].start < tokens[object].end; ++i) {
        if (tokens[i].parent != object) continue;
        if (tokens[i].type != JSMN_STRING || tokens[i].size != 1 || i + 1 >= count) return -2;
        if (token_is(json, &tokens[i], key)) {
            if (found != -1) return -2;
            found = i + 1;
        }
    }
    return found;
}
static int hex4(const char *p, uint32_t *value)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)p[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10u;
        else return AI_DS_ERR_JSON;
        v = (v << 4) | digit;
    }
    *value = v;
    return 0;
}
static int decoded_string(const char *json, const jsmntok_t *token,
                          char *out, size_t capacity, size_t *bytes)
{
    size_t read = (size_t)token->start, end = (size_t)token->end, written = 0;
    if (token->type != JSMN_STRING) return AI_DS_ERR_JSON;
    while (read < end) {
        uint32_t cp;
        size_t used;
        unsigned char c = (unsigned char)json[read];
        if (c == '\\') {
            if (++read == end) return AI_DS_ERR_JSON;
            c = (unsigned char)json[read++];
            if (c == 'u') {
                if (end - read < 4 || hex4(json + read, &cp)) return AI_DS_ERR_JSON;
                read += 4;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t low;
                    if (end - read < 6 || json[read] != '\\' || json[read + 1] != 'u' ||
                        hex4(json + read + 2, &low) || low < 0xdc00 || low > 0xdfff)
                        return AI_DS_ERR_UTF8;
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + low - 0xdc00u;
                    read += 6;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) return AI_DS_ERR_UTF8;
            } else {
                switch (c) {
                case '"': case '\\': case '/': cp = c; break;
                case 'b': cp = 8; break; case 'f': cp = 12; break;
                case 'n': cp = 10; break; case 'r': cp = 13; break;
                case 't': cp = 9; break; default: return AI_DS_ERR_JSON;
                }
            }
        } else {
            if (c < 32) return AI_DS_ERR_JSON;
            if (utf8_codepoint((const unsigned char *)json + read, end - read, &cp, &used))
                return AI_DS_ERR_UTF8;
            read += used;
        }
        if (!cp) return AI_DS_ERR_UTF8;
        used = cp < 0x80 ? 1u : cp < 0x800 ? 2u : cp < 0x10000 ? 3u : 4u;
        if (written >= capacity || used >= capacity - written) return AI_DS_ERR_CAPACITY;
        if (used == 1) out[written++] = (char)cp;
        else {
            if (used == 2) out[written++] = (char)(0xc0u | (cp >> 6));
            else {
                if (used == 3) out[written++] = (char)(0xe0u | (cp >> 12));
                else {
                    out[written++] = (char)(0xf0u | (cp >> 18));
                    out[written++] = (char)(0x80u | ((cp >> 12) & 63u));
                }
                out[written++] = (char)(0x80u | ((cp >> 6) & 63u));
            }
            out[written++] = (char)(0x80u | (cp & 63u));
        }
    }
    out[written] = 0; *bytes = written;
    return 0;
}
static int unsigned_token(const char *json, const jsmntok_t *t, uint32_t *out)
{
    uint32_t value = 0;
    if (t->type != JSMN_PRIMITIVE || t->end == t->start) return AI_DS_ERR_JSON;
    for (int i = t->start; i < t->end; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (c < '0' || c > '9' || value > (UINT32_MAX - (uint32_t)(c - '0')) / 10u)
            return AI_DS_ERR_JSON;
        value = value * 10u + (uint32_t)(c - '0');
    }
    *out = value;
    return 0;
}
static int read_usage(ai_deepseek_stream *s, const jsmntok_t *tokens, int count, int index)
{
    static const char *names[] = {"prompt_tokens", "completion_tokens", "total_tokens",
                                 "prompt_cache_hit_tokens", "prompt_cache_miss_tokens"};
    uint32_t *values[] = {&s->usage.prompt_tokens, &s->usage.completion_tokens, &s->usage.total_tokens,
                         &s->usage.cache_hit_tokens, &s->usage.cache_miss_tokens};
    memset(&s->usage, 0, sizeof(s->usage));
    if (tokens[index].type != JSMN_OBJECT) return AI_DS_ERR_JSON;
    for (unsigned i = 0; i < 5; ++i) {
        int at = field(s->event, tokens, count, index, names[i]);
        if (at == -2) return AI_DS_ERR_JSON;
        if (at >= 0) {
            int rc = unsigned_token(s->event, &tokens[at], values[i]);
            if (rc) return rc;
            s->usage.present |= 1u << i;
        }
    }
    return 0;
}

static int parse_event(ai_deepseek_stream *s)
{
    jsmntok_t tokens[JSON_TOKENS];
    jsmn_parser parser;
    int count, at, choices, choice, delta, rc;
    size_t bytes;
    if (s->event_bytes == 6 && !memcmp(s->event, "[DONE]", 6)) {
        s->saw_done = 1;
        if (s->saw_finish && !strcmp(s->finish_reason, "stop") && s->answer_bytes)
            s->status = AI_DS_COMPLETE;
        else if (s->saw_finish && !strcmp(s->finish_reason, "length")) s->status = AI_DS_LIMIT;
        else s->status = AI_DS_FAILED;
        (void)notify(s, AI_DS_FINISHED, NULL, 0);
        return 0;
    }
    jsmn_init(&parser);
    count = jsmn_parse(&parser, s->event, s->event_bytes, tokens, JSON_TOKENS);
    if (count == JSMN_ERROR_NOMEM) return AI_DS_ERR_CAPACITY;
    if (count < 1 || tokens[0].type != JSMN_OBJECT) return AI_DS_ERR_JSON;
    for (size_t i = (size_t)tokens[0].end; i < s->event_bytes; ++i)
        if (s->event[i] != ' ' && s->event[i] != '\t' && s->event[i] != '\r' && s->event[i] != '\n')
            return AI_DS_ERR_JSON;
    at = field(s->event, tokens, count, 0, "error");
    if (at == -2) return AI_DS_ERR_JSON;
    if (at >= 0 && !token_null(s->event, &tokens[at])) return AI_DS_ERR_SERVER;
    at = field(s->event, tokens, count, 0, "id");
    if (at == -2) return AI_DS_ERR_JSON;
    if (at >= 0) {
        rc = decoded_string(s->event, &tokens[at], s->text, sizeof(s->response_id), &bytes);
        if (rc) return rc;
        if (s->response_id[0] && strcmp(s->response_id, s->text)) return AI_DS_ERR_JSON;
        memcpy(s->response_id, s->text, bytes + 1);
    }
    at = field(s->event, tokens, count, 0, "usage");
    if (at == -2) return AI_DS_ERR_JSON;
    if (at >= 0 && !token_null(s->event, &tokens[at])) {
        rc = read_usage(s, tokens, count, at);
        if (rc) return rc;
    }
    choices = field(s->event, tokens, count, 0, "choices");
    if (choices < 0 || tokens[choices].type != JSMN_ARRAY || tokens[choices].size > 1)
        return AI_DS_ERR_JSON;
    if (!tokens[choices].size) return 0; /* Compatibility with usage-only chunks. */
    choice = choices + 1;
    delta = field(s->event, tokens, count, choice, "delta");
    if (delta == -2) return AI_DS_ERR_JSON;
    if (delta >= 0) {
        at = field(s->event, tokens, count, delta, "content");
        if (at == -2) return AI_DS_ERR_JSON;
        if (at >= 0 && !token_null(s->event, &tokens[at])) {
            rc = decoded_string(s->event, &tokens[at], s->text, sizeof(s->text), &bytes);
            if (rc) return rc;
            if (bytes) {
                if (s->saw_finish) return AI_DS_ERR_JSON;
                if (bytes > AI_ANSWER_BYTES - s->answer_bytes) return AI_DS_ERR_CAPACITY;
                if (notify(s, AI_DS_TEXT, s->text, bytes)) return AI_DS_ERR_CALLBACK;
                s->answer_bytes += bytes;
            }
        }
    }
    at = field(s->event, tokens, count, choice, "finish_reason");
    if (at == -2) return AI_DS_ERR_JSON;
    if (at >= 0 && !token_null(s->event, &tokens[at])) {
        if (s->saw_finish) return AI_DS_ERR_JSON;
        rc = decoded_string(s->event, &tokens[at], s->finish_reason, sizeof(s->finish_reason), &bytes);
        if (rc) return rc;
        s->saw_finish = 1;
    }
    return 0;
}

static int line_end(ai_deepseek_stream *s)
{
    size_t n = s->line_bytes, start;
    int rc;
    if (n && s->line[n - 1] == '\r') --n;
    if (!n) {
        if (!s->event_bytes) return 0;
        s->event[s->event_bytes] = 0;
        rc = parse_event(s);
        s->event_bytes = 0;
        return rc;
    }
    if (n < 5 || memcmp(s->line, "data:", 5)) return 0;
    start = 5;
    if (start < n && s->line[start] == ' ') ++start;
    if (s->event_bytes) {
        if (s->event_bytes == AI_DEEPSEEK_EVENT_BYTES) return AI_DS_ERR_CAPACITY;
        s->event[s->event_bytes++] = '\n';
    }
    if (n - start > AI_DEEPSEEK_EVENT_BYTES - s->event_bytes) return AI_DS_ERR_CAPACITY;
    memcpy(s->event + s->event_bytes, s->line + start, n - start);
    s->event_bytes += n - start;
    return 0;
}
int ai_deepseek_stream_feed(ai_deepseek_stream *s, uint32_t generation,
                            const void *data, size_t bytes)
{
    const unsigned char *p = data;
    if (!s || (!data && bytes)) return AI_DS_ERR_ARGUMENT;
    if (generation != s->generation || s->status != AI_DS_STREAMING) return AI_DS_IGNORED;
    for (size_t i = 0; i < bytes; ++i) {
        if (p[i] == '\n') {
            int rc = line_end(s);
            s->line_bytes = 0;
            if (rc) return fail(s, rc);
            if (s->status != AI_DS_STREAMING) return 0;
        } else {
            if (s->line_bytes == AI_DEEPSEEK_EVENT_BYTES) return fail(s, AI_DS_ERR_CAPACITY);
            if (!p[i]) return fail(s, AI_DS_ERR_JSON);
            s->line[s->line_bytes++] = (char)p[i];
        }
    }
    return 0;
}
int ai_deepseek_stream_end(ai_deepseek_stream *s, uint32_t generation)
{
    if (!s) return AI_DS_ERR_ARGUMENT;
    if (generation != s->generation || s->status != AI_DS_STREAMING) return AI_DS_IGNORED;
    return fail(s, AI_DS_ERR_MISSING_DONE);
}
int ai_deepseek_stream_cancel(ai_deepseek_stream *s, uint32_t generation)
{
    if (!s) return AI_DS_ERR_ARGUMENT;
    if (generation != s->generation || s->status != AI_DS_STREAMING) return AI_DS_IGNORED;
    s->status = AI_DS_CANCELLED;
    (void)notify(s, AI_DS_FINISHED, NULL, 0);
    return 0;
}
