#include "ai_config.h"
#include "ai_history.h"
#include "ai_assets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ai_history history;
static void history_test(void)
{
    ai_history_init(&history);
    const ai_turn *context[8]; uint32_t id = 0;
    for (unsigned i = 1; i <= 60; ++i) {
        char text[20]; snprintf(text, sizeof(text), "%u", i);
        assert(!ai_history_begin(&history, text, &id));
        assert(!ai_history_append(&history, id, "回答", strlen("回答")));
        assert(!ai_history_finish(&history, id, AI_TURN_COMPLETE));
    }
    assert(history.count == 50 && !strcmp(ai_history_at(&history, 0)->user, "11"));
    assert(ai_history_context(&history, context) == 8 && !strcmp(context[0]->user, "53"));
    assert(!ai_history_begin(&history, "重试", &id));
    assert(!ai_history_append(&history, id, "未完成", strlen("未完成")));
    assert(!ai_history_finish(&history, id, AI_TURN_FAILED));
    assert(ai_history_context(&history, context) == 8 && !strcmp(context[7]->user, "60"));
    uint32_t stale = id;
    assert(!ai_history_retry(&history, &id) && stale != id);
    assert(ai_history_at(&history, history.count - 2)->status == AI_TURN_FAILED);
    assert(!strcmp(ai_history_at(&history, history.count - 2)->assistant, "未完成"));
    assert(ai_history_retry(&history, &id) < 0); /* Repeated click cannot create another request. */
    assert(ai_history_append(&history, stale, "old", 3) < 0);
    assert(!ai_history_append(&history, id, "ok", 2));
    assert(!ai_history_finish(&history, id, AI_TURN_COMPLETE));
    ai_history_clear(&history);
    assert(!history.count && !ai_history_context(&history, context));
    assert(!ai_history_begin(&history, "new", &id) && id != stale);
    assert(ai_history_append(&history, stale, "old", 3) < 0);
    char *large = malloc(AI_ANSWER_BYTES + 1); assert(large);
    memset(large, 'a', AI_ANSWER_BYTES);
    assert(!ai_history_append(&history, id, large, AI_ANSWER_BYTES));
    assert(ai_history_append(&history, id, "b", 1) < 0);
    free(large);
}
static void config_test(void)
{
    ai_config config; unsigned line;
    const char *good = "provider=deepseek\r\napi_key=test-key\nmodel=deepseek-flash\nwifi_ssid=测试\nwifi_password=abcd=1234\n";
    assert(!ai_config_parse(&config, good, strlen(good), &line));
    assert(config.provider == AI_PROVIDER_DEEPSEEK && !strcmp(config.wifi_password, "abcd=1234"));
    const char *bad[] = {"api_key=REPLACE_ME", "api_key=a\napi_key=b", "provider=bailian\napi_key=test",
                         "api_key=has space", "api_key=ok\nunknown=value"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        assert(ai_config_parse(&config, bad[i], strlen(bad[i]), &line) < 0);
        assert(!config.api_key[0]);
    }
}
static void independent_context_test(void)
{
    ai_history_clear(&history);
    const ai_turn *context[AI_CONTEXT_ROUNDS];
    uint32_t id = 0;
    for (unsigned i = 1; i <= AI_CONTEXT_ROUNDS; ++i) {
        char question[20]; snprintf(question, sizeof(question), "complete-%u", i);
        assert(!ai_history_begin(&history, question, &id));
        assert(!ai_history_append(&history, id, "answer", 6));
        assert(!ai_history_finish(&history, id, AI_TURN_COMPLETE));
    }
    /* Every successful turn leaves the display ring, but stays in context. */
    for (unsigned i = 0; i < AI_HISTORY_ROUNDS; ++i) {
        char question[20]; snprintf(question, sizeof(question), "failed-%u", i);
        assert(!ai_history_begin(&history, question, &id));
        assert(!ai_history_append(&history, id, "incomplete", 10));
        assert(!ai_history_finish(&history, id, AI_TURN_FAILED));
    }
    assert(history.count == AI_HISTORY_ROUNDS);
    assert(!strcmp(ai_history_at(&history, 0)->user, "failed-0"));
    assert(ai_history_context(&history, context) == AI_CONTEXT_ROUNDS);
    for (unsigned i = 0; i < AI_CONTEXT_ROUNDS; ++i) {
        char question[20]; snprintf(question, sizeof(question), "complete-%u", i + 1);
        assert(!strcmp(context[i]->user, question));
        assert(!strcmp(context[i]->assistant, "answer"));
        assert(context[i]->status == AI_TURN_COMPLETE);
    }
    /* Retried cancellation/limit and active partial output do not enter context. */
    assert(!ai_history_retry(&history, &id));
    assert(!ai_history_finish(&history, id, AI_TURN_CANCELLED));
    assert(!ai_history_retry(&history, &id));
    assert(!ai_history_finish(&history, id, AI_TURN_LIMIT));
    assert(!ai_history_retry(&history, &id));
    assert(!ai_history_append(&history, id, "recovered", 9));
    assert(ai_history_context(&history, context) == AI_CONTEXT_ROUNDS);
    assert(!strcmp(context[0]->user, "complete-1"));
    assert(!ai_history_finish(&history, id, AI_TURN_COMPLETE));
    assert(ai_history_context(&history, context) == AI_CONTEXT_ROUNDS);
    assert(!strcmp(context[0]->user, "complete-2"));
    assert(!strcmp(context[7]->user, "failed-49"));
    assert(!strcmp(context[7]->assistant, "recovered"));
    uint32_t old = id;
    ai_history_clear(&history);
    assert(!history.count && !history.model_count && !ai_history_context(&history, context));
    assert(history.next_id == old);
    assert(!ai_history_begin(&history, "new session", &id) && id != old);
    assert(ai_history_append(&history, old, "late", 4) < 0);
    assert(!ai_history_finish(&history, id, AI_TURN_CANCELLED));
    printf("History bytes: display=%zu, model=%zu, total=%zu\n",
           sizeof(history.turns), sizeof(history.model_turns), sizeof(history));
}
static void assets_test(const char *path, int font)
{
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long length = ftell(file); assert(length > 16);
    rewind(file); unsigned char *data = malloc((size_t)length); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length); fclose(file);
    ai_asset resource;
    assert(!ai_asset_open(&resource, data, (size_t)length, font ? "AIF1" : "AIP1"));
    if (font) {
        assert(!ai_font_validate(&resource)); assert(ai_font_find(&resource, 0x4e2d));
        assert(!ai_font_find(&resource, 0x10000));
    } else {
        const char *values[8];
        assert(!ai_dictionary_validate(&resource));
        unsigned count = ai_dictionary_query(&resource, "ni'hao", 0, values, 8);
        assert(count && !strcmp(values[0], "你好"));
        assert(ai_dictionary_query(&resource, "zhongguo", 0, values, 8));
        assert(!ai_dictionary_query(&resource, "123", 0, values, 8));
    }
    data[length-1] ^= 1;
    assert(ai_asset_open(&resource, data, (size_t)length, font ? "AIF1" : "AIP1") < 0);
    free(data);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    history_test(); independent_context_test(); config_test();
    assets_test(argv[1], 1); assets_test(argv[2], 0);
    puts("AI core: history, stale events, retry, config, font and phrase dictionary passed");
    return 0;
}
