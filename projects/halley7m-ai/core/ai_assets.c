#include "ai_assets.h"
#include <errno.h>
#include <string.h>
static uint32_t u32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint32_t ai_crc32(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (bytes--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
int ai_asset_open(ai_asset *out, const void *data, size_t bytes, const char magic[4])
{
    const uint8_t *p = data;
    if (!out || !p || bytes < 16 || memcmp(p, magic, 4)) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (u32(p + 8) != bytes - 16 || u32(p + 12) != ai_crc32(p + 16, bytes - 16)) return -EINVAL;
    out->count = u32(p + 4); out->payload = p + 16; out->bytes = bytes - 16;
    return 0;
}
int ai_font_validate(const ai_asset *font)
{
    if (!font || font->count > 65536 || font->bytes != (size_t)font->count * 36) return -EINVAL;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < font->count; ++i) {
        const uint8_t *r = font->payload + i * 36;
        uint32_t code = r[0] | (uint32_t)r[1] << 8;
        if ((i && code <= previous) || (r[2] != 8 && r[2] != 16) || r[3]) return -EINVAL;
        previous = code;
    }
    return 0;
}
const uint8_t *ai_font_find(const ai_asset *font, uint32_t code)
{
    if (!font || !font->payload || code > 65535) return NULL;
    uint32_t lo = 0, hi = font->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *r = font->payload + mid * 36;
        uint32_t current = r[0] | (uint32_t)r[1] << 8;
        if (current == code) return r;
        if (current < code) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static const char *dictionary_string(const ai_asset *dict, uint32_t index, unsigned field)
{ return (const char *)dict->payload + u32(dict->payload + index * 12 + field); }
int ai_dictionary_validate(const ai_asset *dict)
{
    if (!dict || dict->bytes > 2u * 1024u * 1024u || dict->count > dict->bytes / 12) return -EINVAL;
    const char *previous = "";
    uint32_t last_weight = UINT32_MAX;
    for (uint32_t i = 0; i < dict->count; ++i) {
        for (unsigned field = 0; field < 8; field += 4) {
            uint32_t off = u32(dict->payload + i * 12 + field);
            if (off < dict->count * 12 || off >= dict->bytes ||
                !memchr(dict->payload + off, 0, dict->bytes - off)) return -EINVAL;
        }
        const char *key = dictionary_string(dict, i, 0);
        if (!*key || !*dictionary_string(dict, i, 4)) return -EINVAL;
        for (const char *p = key; *p; ++p) if (*p < 'a' || *p > 'z') return -EINVAL;
        int order = strcmp(previous, key);
        uint32_t weight = u32(dict->payload + i * 12 + 8);
        if (order > 0 || (i && !order && weight > last_weight)) return -EINVAL;
        previous = key; last_weight = weight;
    }
    return 0;
}
unsigned ai_dictionary_query(const ai_asset *dict, const char *pinyin,
                             unsigned offset, const char **candidates, unsigned capacity)
{
    char key[65]; size_t len = 0;
    if (!dict || !dict->payload) return 0;
    for (; *pinyin; ++pinyin) {
        char c = *pinyin;
        if (c == '\'' || c == ' ') continue;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c < 'a' || c > 'z' || len == sizeof(key) - 1) return 0;
        key[len++] = c;
    }
    key[len] = 0;
    if (!len) return 0;
    uint32_t lo = 0, hi = dict->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(dictionary_string(dict, mid, 0), key) < 0) lo = mid + 1; else hi = mid;
    }
    unsigned count = 0;
    while (lo < dict->count && !strcmp(dictionary_string(dict, lo, 0), key)) {
        if (offset) --offset;
        else if (count < capacity) candidates[count++] = dictionary_string(dict, lo, 4);
        else break;
        ++lo;
    }
    return count;
}
