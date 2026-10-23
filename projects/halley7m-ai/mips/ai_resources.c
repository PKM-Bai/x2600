#include "ai_resources.h"
#include "player_storage.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int read_file(const char *path, void **data, size_t *bytes, size_t maximum)
{
    struct player_storage_file *file = NULL;
    int result = player_storage_open_utf8(&file, path);
    if (result) return result;
    const struct player_media_io *io = player_storage_io(file);
    uint64_t size = io->size(io->opaque);
    if (!size || size > maximum) result = -E2BIG;
    unsigned char *buffer = result ? NULL : malloc((size_t)size + 1);
    if (!result && !buffer) result = -ENOMEM;
    size_t offset = 0;
    while (!result && offset < size) {
        uint32_t need = (uint32_t)(size - offset);
        if (need > 16384) need = 16384;
        int32_t got = io->read(io->opaque, buffer + offset, need);
        if (got <= 0 || got > (int32_t)need) result = -EIO;
        else offset += (size_t)got;
    }
    int closed = player_storage_close(file);
    if (!result) result = closed;
    if (result) { free(buffer); return result; }
    buffer[offset] = 0;
    *data = buffer; *bytes = offset;
    return 0;
}
static int resource(const char *path, const char *magic, ai_asset *out, size_t maximum, int font)
{
    void *data = NULL; size_t bytes = 0;
    int result = read_file(path, &data, &bytes, maximum);
    if (!result) result = ai_asset_open(out, data, bytes, magic);
    if (!result) result = font ? ai_font_validate(out) : ai_dictionary_validate(out);
    if (result) { free(data); memset(out, 0, sizeof(*out)); }
    return result;
}
void ai_resources_load(ai_resources *r)
{
    memset(r, 0, sizeof(*r));
    r->mount_result = player_storage_mount();
    if (r->mount_result) {
        r->font_result = r->dictionary_result = r->config_result = r->mount_result;
        return;
    }
    r->font_result = resource("/ai/font.aif", "AIF1", &r->font, 2u * 1024u * 1024u, 1);
    r->dictionary_result = resource("/ai/pinyin.aip", "AIP1", &r->dictionary, 2u * 1024u * 1024u, 0);
    void *config = NULL; size_t size = 0;
    r->config_result = read_file("/ai/config.ini", &config, &size, 8192);
    if (!r->config_result) r->config_result = ai_config_parse(&r->config, config, size, &r->config_error_line);
    if (config) { memset(config, 0, size); free(config); }
}
