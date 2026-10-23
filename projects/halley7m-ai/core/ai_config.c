#include "ai_config.h"
#include <errno.h>
#include <string.h>

int ai_config_parse(ai_config *out, const char *data, size_t bytes, unsigned *error_line)
{
    unsigned line = 0, seen = 0;
    size_t pos = 0;
    ai_config parsed = {0};
    if (!out || !data || bytes > 8192) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (error_line) *error_line = 0;
    if (bytes >= 3 && !memcmp(data, "\xef\xbb\xbf", 3)) pos = 3;
    while (pos < bytes) {
        size_t start = pos, end, split;
        char *dest = NULL;
        size_t capacity = 0;
        unsigned bit = 0;
        ++line;
        while (pos < bytes && data[pos] != '\n') ++pos;
        end = pos;
        if (pos < bytes) ++pos;
        if (end > start && data[end - 1] == '\r') --end;
        if (start == end || data[start] == '#') continue;
        if (memchr(data + start, 0, end - start)) goto invalid;
        split = start;
        while (split < end && data[split] != '=') ++split;
        if (split == end) goto invalid;
#define KEY(name) (split - start == sizeof(name) - 1 && !memcmp(data + start, name, sizeof(name) - 1))
        if (KEY("provider")) {
            size_t len = end - split - 1;
            if (seen & 1) goto invalid;
            seen |= 1;
            if (len == 8 && !memcmp(data + split + 1, "deepseek", 8)) parsed.provider = AI_PROVIDER_DEEPSEEK;
            else if (len == 7 && !memcmp(data + split + 1, "bailian", 7)) parsed.provider = AI_PROVIDER_BAILIAN;
            else goto invalid;
            continue;
        }
#define FIELD(key, member, flag) if (KEY(key)) { dest = parsed.member; capacity = sizeof(parsed.member); bit = flag; }
        FIELD("model", model, 2)
        else FIELD("api_key", api_key, 4)
        else FIELD("workspace_id", workspace_id, 8)
        else FIELD("wifi_ssid", wifi_ssid, 16)
        else FIELD("wifi_password", wifi_password, 32)
        else goto invalid;
#undef FIELD
#undef KEY
        if ((seen & bit) || end - split - 1 >= capacity) goto invalid;
        seen |= bit;
        memcpy(dest, data + split + 1, end - split - 1);
        dest[end - split - 1] = 0;
    }
    if (!parsed.model[0]) strcpy(parsed.model, parsed.provider == AI_PROVIDER_DEEPSEEK ?
                                "deepseek-flash" : "qwen3.8-omni-flash-realtime");
    if (!parsed.api_key[0] || !strcmp(parsed.api_key, "REPLACE_ME") ||
        (parsed.provider == AI_PROVIDER_BAILIAN && !parsed.workspace_id[0])) goto invalid;
    /* Credentials become HTTP headers/URL components, never allow controls. */
    for (size_t i = 0; parsed.api_key[i]; ++i)
        if ((unsigned char)parsed.api_key[i] <= 32 || (unsigned char)parsed.api_key[i] >= 127) goto invalid;
    for (size_t i = 0; parsed.workspace_id[i]; ++i) {
        char c = parsed.workspace_id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-')) goto invalid;
    }
    *out = parsed;
    return 0;
invalid:
    if (error_line) *error_line = line;
    return -EINVAL;
}
