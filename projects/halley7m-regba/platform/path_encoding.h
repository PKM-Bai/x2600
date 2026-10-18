/* UTF-8 application paths / FatFs UTF-16 boundary. GPL-2.0-or-later. */
#ifndef REGBA_PATH_ENCODING_H
#define REGBA_PATH_ENCODING_H

#include <stddef.h>
#include <stdint.h>

/* Capacities include the terminator and count output elements, not bytes.
 * Return 0, -EILSEQ for malformed input, -ENAMETOOLONG for insufficient
 * output, or -EINVAL for missing buffers. On failure a valid output buffer
 * is an empty string; conversion never returns a truncated pathname. */
int utf8_to_utf16(uint16_t *out, size_t capacity, const char *input);
int utf16_to_utf8(char *out, size_t capacity, const uint16_t *input);

#endif
