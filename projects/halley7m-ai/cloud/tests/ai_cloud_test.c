#include "ai_deepseek.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ai_history history;
static ai_deepseek_stream stream;
static char request[100000];
static unsigned tests_run;

typedef struct {
    char text[AI_ANSWER_BYTES + 1];
    size_t bytes;
    unsigned texts, finished;
    uint32_t generation;
    ai_deepseek_status status;
    ai_deepseek_error error;
    ai_deepseek_usage usage;
    int reject_text, reject_finished;
    ai_history *history;
} collector;
static collector received;

static int collect(void *user, const ai_deepseek_event *event)
{
    collector *c = user;
    assert(event->generation == c->generation);
    if (event->kind == AI_DS_TEXT) {
        if (c->reject_text) return -1;
        assert(event->status == AI_DS_STREAMING);
        assert(event->text && event->text_bytes);
        assert(event->text_bytes <= AI_ANSWER_BYTES - c->bytes);
        memcpy(c->text + c->bytes, event->text, event->text_bytes);
        c->bytes += event->text_bytes;
        c->text[c->bytes] = 0;
        ++c->texts;
        if (c->history)
            assert(!ai_history_append(c->history, event->generation, event->text, event->text_bytes));
        return 0;
    }
    assert(event->kind == AI_DS_FINISHED);
    assert(!event->text && !event->text_bytes);
    ++c->finished;
    assert(c->finished == 1);
    c->status = event->status;
    c->error = event->error;
    c->usage = event->usage;
    if (c->history) {
        ai_turn_status status = AI_TURN_FAILED;
        if (event->status == AI_DS_COMPLETE) status = AI_TURN_COMPLETE;
        else if (event->status == AI_DS_CANCELLED) status = AI_TURN_CANCELLED;
        else if (event->status == AI_DS_LIMIT) status = AI_TURN_LIMIT;
        assert(!ai_history_finish(c->history, event->generation, status));
    }
    return c->reject_finished;
}

static void reset(uint32_t generation)
{
    memset(&received, 0, sizeof(received));
    received.generation = generation;
    assert(!ai_deepseek_stream_init(&stream, generation, collect, &received));
}
static int feed(const char *s)
{ return ai_deepseek_stream_feed(&stream, stream.generation, s, strlen(s)); }

static const char text_chunk[] =
    "data: {\"id\":\"sample\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"中文\\ud83d\\ude80\"},"
    "\"finish_reason\":null}],\"usage\":null}\r\n\r\n";
static const char stop_chunk[] =
    "data: {\"id\":\"sample\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":29,\"completion_tokens\":3,"
    "\"total_tokens\":32,\"prompt_cache_hit_tokens\":0,\"prompt_cache_miss_tokens\":29}}\n\n";
static const char stop_no_usage[] =
    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
static const char done[] = "data: [DONE]\n\n";

typedef struct { size_t bytes, capacity; char *data; } output;
static int write_output(void *user, const char *data, size_t bytes)
{
    output *out = user;
    if (bytes > out->capacity - out->bytes) return -1;
    if (out->data) memcpy(out->data + out->bytes, data, bytes);
    out->bytes += bytes;
    return 0;
}
static unsigned occurrences(const char *s, const char *word)
{
    unsigned count = 0;
    size_t bytes = strlen(word);
    while ((s = strstr(s, word))) { ++count; s += bytes; }
    return count;
}
static uint32_t begin(const char *text)
{
    uint32_t id = 0;
    assert(!ai_history_begin(&history, text, &id));
    assert(id);
    return id;
}
static void pair(const char *user, const char *assistant)
{
    uint32_t id = begin(user);
    assert(!ai_history_append(&history, id, assistant, strlen(assistant)));
    assert(!ai_history_finish(&history, id, AI_TURN_COMPLETE));
}

static void test_request(void)
{
    size_t measured, written;
    uint32_t id;
    output out = {0, sizeof(request) - 1, request};
    ai_history_init(&history);
    for (unsigned i = 1; i <= 10; ++i) {
        char user[32], answer[32];
        snprintf(user, sizeof(user), "USER%02u", i);
        snprintf(answer, sizeof(answer), "ANSWER%02u", i);
        pair(user, answer);
    }
    id = begin("failed turn");
    assert(!ai_history_finish(&history, id, AI_TURN_FAILED));
    id = begin("中文\"\\\n\t\001");
    assert(!ai_deepseek_write_request(&history, id, NULL, NULL, &measured));
    assert(!ai_deepseek_write_request(&history, id, write_output, &out, &written));
    assert(measured == written && written == out.bytes);
    request[written] = 0;
    assert(!strncmp(request, "{\"model\":\"deepseek-flash\",", 26));
    assert(strstr(request, "\"thinking\":{\"type\":\"disabled\"}"));
    assert(strstr(request, "\"max_tokens\":2048,\"stream\":true"));
    assert(strstr(request, "\"stream_options\":{\"include_usage\":true}"));
    assert(occurrences(request, "\"role\":\"user\"") == 9);
    assert(occurrences(request, "\"role\":\"assistant\"") == 8);
    assert(occurrences(request, "\"role\":\"system\"") == 1);
    assert(!strstr(request, "USER01") && !strstr(request, "USER02"));
    assert(strstr(request, "USER03") < strstr(request, "ANSWER03"));
    assert(strstr(request, "ANSWER03") < strstr(request, "USER04"));
    assert(strstr(request, "USER10"));
    assert(!strstr(request, "failed turn"));
    assert(strstr(request, "中文\\\"\\\\\\u000a\\u0009\\u0001"));
    assert(!strcmp(request + written - 3, "}]}"));
    out.bytes = 0;
    assert(ai_deepseek_write_request(&history, id - 1, write_output, &out, &written) == AI_DS_ERR_STALE);
    assert(!written && !out.bytes);
    out.capacity = 30;
    assert(ai_deepseek_write_request(&history, id, write_output, &out, &written) == AI_DS_ERR_WRITE);
    ai_history_clear(&history);
    id = begin("\xc0\xaf");
    assert(ai_deepseek_write_request(&history, id, NULL, NULL, &measured) == AI_DS_ERR_UTF8);
    ++tests_run;
}

static void test_max_request(void)
{
    static char user[AI_INPUT_BYTES + 1], answer[AI_ANSWER_BYTES + 1];
    size_t measured, written;
    output out = {0, SIZE_MAX, NULL};
    uint32_t id;
    memset(user, 1, AI_INPUT_BYTES); user[AI_INPUT_BYTES] = 0;
    memset(answer, 2, AI_ANSWER_BYTES); answer[AI_ANSWER_BYTES] = 0;
    ai_history_clear(&history);
    for (unsigned i = 0; i < AI_CONTEXT_ROUNDS; ++i) pair(user, answer);
    id = begin(user);
    assert(!ai_deepseek_write_request(&history, id, NULL, NULL, &measured));
    assert(measured > 1700000);
    assert(!ai_deepseek_write_request(&history, id, write_output, &out, &written));
    assert(measured == written && out.bytes == written);
    ++tests_run;
}

static void verify_success(const char *expected)
{
    assert(stream.status == AI_DS_COMPLETE);
    assert(stream.saw_done && stream.saw_finish);
    assert(received.finished == 1 && received.status == AI_DS_COMPLETE);
    assert(!strcmp(received.text, expected));
    assert(ai_deepseek_usage_complete(&received.usage));
    assert(received.usage.prompt_tokens == 29 && received.usage.completion_tokens == 3);
    assert(received.usage.total_tokens == 32 && received.usage.present == 31u);
}
static void test_all_splits(void)
{
    char data[2048];
    size_t bytes;
    snprintf(data, sizeof(data), ": keepalive\r\n\r\n%s%s%s", text_chunk, stop_chunk, done);
    bytes = strlen(data);
    for (size_t split = 0; split < bytes; ++split) {
        reset(101);
        assert(!ai_deepseek_stream_feed(&stream, 101, data, split));
        assert(!ai_deepseek_stream_feed(&stream, 101, data + split, bytes - split));
        verify_success("中文🚀");
    }
    for (size_t step = 1; step <= 17; ++step) {
        reset(102);
        for (size_t i = 0; i < bytes; i += step) {
            size_t n = bytes - i < step ? bytes - i : step;
            assert(!ai_deepseek_stream_feed(&stream, 102, data + i, n));
        }
        verify_success("中文🚀");
    }
    ++tests_run;
}

static void test_usage_and_delayed_done(void)
{
    reset(1);
    assert(!feed(text_chunk));
    assert(!feed(stop_no_usage));
    assert(stream.status == AI_DS_STREAMING && !received.finished);
    assert(!feed(": waiting\n\n"));
    assert(!feed("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":29,\"completion_tokens\":3,"
                 "\"total_tokens\":32,\"prompt_cache_hit_tokens\":0,\"prompt_cache_miss_tokens\":29}}\n\n"));
    assert(!feed(done));
    verify_success("中文🚀");
    reset(2);
    assert(!feed(text_chunk) && !feed(stop_no_usage) && !feed(done));
    assert(stream.status == AI_DS_COMPLETE && !ai_deepseek_usage_complete(&received.usage));
    assert(received.usage.present == 0);
    reset(3);
    assert(!feed(text_chunk) && !feed(stop_no_usage));
    assert(!feed("data: {\"choices\":[],\"usage\":{\"total_tokens\":0}}\n\n"));
    assert(!feed(done));
    assert(received.usage.present == AI_DS_USAGE_TOTAL && !ai_deepseek_usage_complete(&received.usage));
    ++tests_run;
}

static void test_sse_and_unicode(void)
{
    reset(1);
    assert(!feed("event: ignored\r\n: comment\r\ndata: {\"choices\":[{\"delta\":\r\n"
                 "data: {\"content\":\"\\u4e2d文\\ud83d\\ude80\\n\\t\\\"\\\\/\"},\"finish_reason\":null}]}\r\n\r\n"));
    assert(!strcmp(received.text, "中文🚀\n\t\"\\/"));
    assert(!feed(stop_no_usage) && !feed(done));
    assert(stream.status == AI_DS_COMPLETE);
    reset(2);
    assert(!feed("data: {\"choices\":[{\"delta\":{\"content\":\"null\"},\"finish_reason\":null}]}\n\n"));
    assert(!strcmp(received.text, "null"));
    assert(!feed(stop_no_usage) && !feed(done));
    assert(stream.status == AI_DS_COMPLETE);
    ++tests_run;
}

static void test_truncation_and_errors(void)
{
    const char *reasons[] = {"length", "aborted", "content_filter", "insufficient_system_resource"};
    for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        char chunk[256];
        reset(i + 1);
        assert(!feed(text_chunk));
        snprintf(chunk, sizeof(chunk), "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"%s\"}]}\n\n", reasons[i]);
        assert(!feed(chunk) && !feed(done));
        assert(stream.status == (i ? AI_DS_FAILED : AI_DS_LIMIT));
        assert(!ai_deepseek_usage_complete(&received.usage));
    }
    reset(5);
    assert(!feed(text_chunk) && !feed(stop_chunk));
    assert(ai_deepseek_stream_end(&stream, 5) == AI_DS_ERR_MISSING_DONE);
    assert(received.status == AI_DS_FAILED && received.error == AI_DS_ERR_MISSING_DONE);
    assert(ai_deepseek_stream_feed(&stream, 5, done, strlen(done)) == AI_DS_IGNORED);
    reset(6);
    assert(!feed(done));
    assert(received.status == AI_DS_FAILED && received.finished == 1);
    reset(7);
    assert(feed("data: {\"error\":{\"message\":\"unavailable\"}}\n\n") == AI_DS_ERR_SERVER);
    assert(received.status == AI_DS_FAILED);
    ++tests_run;
}

static void test_cancel_and_generation(void)
{
    reset(15);
    assert(ai_deepseek_stream_feed(&stream, 14, text_chunk, strlen(text_chunk)) == AI_DS_IGNORED);
    assert(ai_deepseek_stream_cancel(&stream, 14) == AI_DS_IGNORED);
    assert(ai_deepseek_stream_end(&stream, 14) == AI_DS_IGNORED);
    assert(!received.bytes && stream.status == AI_DS_STREAMING);
    assert(!feed(text_chunk));
    assert(!ai_deepseek_stream_cancel(&stream, 15));
    assert(received.status == AI_DS_CANCELLED && received.finished == 1);
    assert(feed(stop_chunk) == AI_DS_IGNORED && feed(done) == AI_DS_IGNORED);
    assert(ai_deepseek_stream_cancel(&stream, 15) == AI_DS_IGNORED);
    reset(16);
    assert(ai_deepseek_stream_feed(&stream, 15, done, strlen(done)) == AI_DS_IGNORED);
    assert(!feed(text_chunk) && !feed(stop_chunk) && !feed(done));
    verify_success("中文🚀");
    ++tests_run;
}

static void test_invalid_json_and_utf8(void)
{
    const char *bad[] = {
        "data: {broken}\n\n",
        "data: {} {}\n\n",
        "data: {\"choices\":[],\"choices\":[]}\n\n",
        "data: {\"choices\":[],\"usage\":\"null\"}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":true}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\u0000\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\ud800\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\udc00\"}}]}\n\n",
        "data: {\"choices\":[{\"delta\":{\"content\":\"\xc0\xaf\"}}]}\n\n",
        "data: {\"choices\":[],\"usage\":{\"total_tokens\":4294967296}}\n\n",
        "data: {\"choices\":[],\"usage\":{\"total_tokens\":-1}}\n\n"
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset(i + 1);
        assert(feed(bad[i]) < 0);
        assert(received.status == AI_DS_FAILED && received.finished == 1);
    }
    reset(99);
    assert(!feed(text_chunk));
    assert(feed("data: {\"id\":\"different\",\"choices\":[]}\n\n") == AI_DS_ERR_JSON);
    reset(100);
    assert(!feed(text_chunk) && !feed(stop_chunk));
    assert(feed(text_chunk) == AI_DS_ERR_JSON);
    ++tests_run;
}

static void test_limits(void)
{
    static char overlong[AI_DEEPSEEK_EVENT_BYTES + 1];
    char chunk[1200], text[1025];
    reset(1);
    memset(overlong, 'a', sizeof(overlong));
    assert(ai_deepseek_stream_feed(&stream, 1, overlong, sizeof(overlong)) == AI_DS_ERR_CAPACITY);
    assert(received.status == AI_DS_LIMIT && received.finished == 1);
    reset(2);
    memset(text, 'x', 1024); text[1024] = 0;
    snprintf(chunk, sizeof(chunk), "data: {\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}\n\n", text);
    for (unsigned i = 0; i < 32; ++i) assert(!feed(chunk));
    assert(received.bytes == AI_ANSWER_BYTES);
    assert(feed(chunk) == AI_DS_ERR_CAPACITY);
    assert(received.bytes == AI_ANSWER_BYTES && received.status == AI_DS_LIMIT);
    ++tests_run;
}

static void test_callbacks_and_history(void)
{
    const ai_turn *context[AI_CONTEXT_ROUNDS];
    uint32_t id;
    reset(1);
    received.reject_text = 1;
    assert(feed(text_chunk) == AI_DS_ERR_CALLBACK);
    assert(received.finished == 1 && !received.bytes && !stream.answer_bytes);
    reset(2);
    received.reject_finished = 1;
    assert(!feed(text_chunk) && !feed(stop_chunk) && !feed(done));
    assert(received.finished == 1 && stream.status == AI_DS_COMPLETE);
    ai_history_clear(&history);
    id = begin("成功回答");
    reset(id); received.history = &history;
    assert(!feed(text_chunk) && !feed(stop_no_usage) && !feed(done));
    assert(ai_history_context(&history, context) == 1);
    assert(!strcmp(context[0]->assistant, "中文🚀"));
    id = begin("取消回答");
    reset(id); received.history = &history;
    assert(!feed(text_chunk));
    assert(!ai_deepseek_stream_cancel(&stream, id));
    assert(ai_history_context(&history, context) == 1);
    assert(!ai_history_retry(&history, &id));
    reset(id); received.history = &history;
    assert(!feed(text_chunk));
    assert(ai_deepseek_stream_end(&stream, id) == AI_DS_ERR_MISSING_DONE);
    assert(ai_history_context(&history, context) == 1);
    ++tests_run;
}

static void test_recorded_shape(const char *path)
{
    FILE *file = fopen(path, "rb");
    int byte;
    assert(file);
    reset(300);
    while ((byte = fgetc(file)) != EOF) {
        unsigned char b = (unsigned char)byte;
        assert(!ai_deepseek_stream_feed(&stream, 300, &b, 1));
    }
    assert(!ferror(file));
    fclose(file);
    verify_success("协议回放好");
    ++tests_run;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_request();
    test_max_request();
    test_all_splits();
    test_usage_and_delayed_done();
    test_sse_and_unicode();
    test_truncation_and_errors();
    test_cancel_and_generation();
    test_invalid_json_and_utf8();
    test_limits();
    test_callbacks_and_history();
    test_recorded_shape(argv[1]);
    printf("PASS: %u test groups; stream=%lu bytes; transport=not implemented\n",
           tests_run, (unsigned long)sizeof(ai_deepseek_stream));
    return 0;
}
