#include "resources.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned char ascii_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int same_directory(const char *path, size_t length, const char *directory)
{
    size_t i;
    if (strlen(directory) != length) return 0;
    for (i = 0; i < length; ++i)
        if (ascii_lower((unsigned char)path[i]) !=
            ascii_lower((unsigned char)directory[i])) return 0;
    return 1;
}

static uint32_t path_crc32(const char *path)
{
    uint32_t crc = UINT32_MAX;
    while (*path) {
        unsigned bit;
        crc ^= ascii_lower((unsigned char)*path++);
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    }
    return crc ^ UINT32_MAX;
}

int regba_backup_path(char *out, size_t capacity, const char *rom_path)
{
    const char *slash, *name, *dot;
    size_t path_length, stem_length, save_name_length, output_length;
    int legacy_name, written;
    if (out && capacity) out[0] = 0;
    if (!out || !rom_path || !rom_path[0]) return -EINVAL;
    for (path_length = 0; path_length <= REGBA_RESOURCE_PATH_MAX; ++path_length)
        if (!rom_path[path_length]) break;
    if (path_length > REGBA_RESOURCE_PATH_MAX) return -ENAMETOOLONG;
    if (!strcmp(rom_path, "/__selftest.gba")) return -ENOTSUP;
    slash = strrchr(rom_path, '/');
    name = slash ? slash + 1 : rom_path;
    dot = strrchr(name, '.');
    stem_length = dot ? (size_t)(dot - name) : strlen(name);
    if (!stem_length) return -EINVAL;
    legacy_name = slash &&
        (same_directory(rom_path, (size_t)(slash - rom_path), REGBA_ROM_DIR) ||
         same_directory(rom_path, (size_t)(slash - rom_path), REGBA_LEGACY_ROM_DIR) ||
         (slash == rom_path && !strncmp(name, "__save_", 7)));
    save_name_length = stem_length + 4u;
    if (!legacy_name) save_name_length += 9u; /* Hyphen and eight CRC32 digits. */
    if (save_name_length > 255u) return -ENAMETOOLONG; /* Current FatFs LFN limit. */
    output_length = strlen(REGBA_SAVE_DIR) + 1u + save_name_length;
    if (output_length > REGBA_RESOURCE_PATH_MAX || capacity <= output_length)
        return -ENAMETOOLONG;
    if (legacy_name)
        written = snprintf(out, capacity, REGBA_SAVE_DIR "/%.*s.sav",
                           (int)stem_length, name);
    else
        written = snprintf(out, capacity, REGBA_SAVE_DIR "/%.*s-%08lx.sav",
                           (int)stem_length, name, (unsigned long)path_crc32(rom_path));
    return written >= 0 && (size_t)written == output_length ? 0 : -EIO;
}
