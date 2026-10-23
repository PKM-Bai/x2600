#ifndef AI_ASSETS_H
#define AI_ASSETS_H
#include <stddef.h>
#include <stdint.h>
typedef struct { const uint8_t *payload; size_t bytes; uint32_t count; } ai_asset;
uint32_t ai_crc32(const void *data, size_t bytes);
int ai_asset_open(ai_asset *out, const void *data, size_t bytes, const char magic[4]);
int ai_font_validate(const ai_asset *font);
const uint8_t *ai_font_find(const ai_asset *font, uint32_t codepoint);
int ai_dictionary_validate(const ai_asset *dictionary);
/* Query complete pinyin, no tones; apostrophes separate syllables. */
unsigned ai_dictionary_query(const ai_asset *dictionary, const char *pinyin,
                             unsigned offset, const char **candidates, unsigned capacity);
#endif
