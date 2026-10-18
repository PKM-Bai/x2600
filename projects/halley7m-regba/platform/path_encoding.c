/* UTF-8 application paths / FatFs UTF-16 boundary. GPL-2.0-or-later. */
#include "path_encoding.h"
#include <errno.h>

int utf8_to_utf16(uint16_t *out, size_t capacity, const char *input)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)input;
    int failure = -EILSEQ;
    if (out && capacity) out[0] = 0;
    if (!out || !capacity || !input) return -EINVAL;
    while (*p) {
        uint32_t code = *p, minimum = 0;
        unsigned bytes = 1;
        if (code >= 0xc2 && code <= 0xdf) {
            code &= 0x1f; bytes = 2; minimum = 0x80;
        } else if (code >= 0xe0 && code <= 0xef) {
            code &= 0x0f; bytes = 3; minimum = 0x800;
        } else if (code >= 0xf0 && code <= 0xf4) {
            code &= 7; bytes = 4; minimum = 0x10000;
        } else if (code >= 0x80) goto fail;
        for (unsigned i = 1; i < bytes; ++i) {
            if ((p[i] & 0xc0) != 0x80) goto fail;
            code = (code << 6) | (p[i] & 0x3f);
        }
        if (code < minimum || code > 0x10ffff ||
                (code >= 0xd800 && code <= 0xdfff)) goto fail;
        size_t units = code >= 0x10000 ? 2u : 1u;
        if (capacity - used <= units) {
            failure = -ENAMETOOLONG;
            goto fail;
        }
        if (units == 2) {
            code -= 0x10000;
            out[used++] = (uint16_t)(0xd800u | (code >> 10));
            out[used++] = (uint16_t)(0xdc00u | (code & 0x3ff));
        } else out[used++] = (uint16_t)code;
        p += bytes;
    }
    out[used] = 0;
    return 0;
fail:
    out[0] = 0;
    return failure;
}

int utf16_to_utf8(char *out, size_t capacity, const uint16_t *input)
{
    size_t used = 0;
    const uint16_t *p = input;
    int failure = -EILSEQ;
    if (out && capacity) out[0] = 0;
    if (!out || !capacity || !input) return -EINVAL;
    while (*p) {
        uint32_t code = *p++;
        if (code >= 0xd800 && code <= 0xdbff) {
            uint32_t low = *p++;
            if (low < 0xdc00 || low > 0xdfff) goto fail;
            code = 0x10000u + ((code - 0xd800u) << 10) + low - 0xdc00u;
        } else if (code >= 0xdc00 && code <= 0xdfff) goto fail;
        unsigned bytes = code < 0x80 ? 1u : code < 0x800 ? 2u : code < 0x10000 ? 3u : 4u;
        if (capacity - used <= bytes) {
            failure = -ENAMETOOLONG;
            goto fail;
        }
        if (bytes == 1) out[used++] = (char)code;
        else {
            unsigned shift = (bytes - 1u) * 6u;
            out[used++] = (char)((bytes == 2 ? 0xc0u : bytes == 3 ? 0xe0u : 0xf0u) | (code >> shift));
            while (shift) {
                shift -= 6;
                out[used++] = (char)(0x80u | ((code >> shift) & 0x3f));
            }
        }
    }
    out[used] = 0;
    return 0;
fail:
    out[0] = 0;
    return failure;
}
