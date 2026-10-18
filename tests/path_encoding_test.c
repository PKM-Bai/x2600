/* Compile the real UTF-8/UTF-16 boundary. Expected UTF-16 code units are
 * explicit uint16_t values, independent of the host's wchar_t width. */
#include "path_encoding.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
static void expect(int condition, const char *what)
{
    if (!condition) { fprintf(stderr, "FAIL path_encoding: %s\n", what); ++failures; }
}

static void check_valid(void)
{
    static const uint16_t empty[] = {0};
    static const uint16_t ascii[] = {'/', 'G', 'B', 'A', '/', 'A', '.', 'g', 'b', 'a', 0};
    static const uint16_t chinese[] = {'/', 0x6e38, 0x620f, '/', 0x53e3, 0x888b,
        0x5996, 0x602a, ' ', 0x7eff, 0x5b9d, 0x77f3, '.', 'g', 'b', 'a', 0};
    static const uint16_t limits[] = {0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff,
        0xd800, 0xdc00, 0xd83c, 0xdfae, 0xdbff, 0xdfff, 0};
    static const struct { const char *utf8; const uint16_t *utf16; size_t units; } cases[] = {
        {"", empty, 0}, {"/GBA/A.gba", ascii, 10},
        {u8"/游戏/口袋妖怪 绿宝石.gba", chinese, 16},
        {"\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf"
         "\xf0\x90\x80\x80\xf0\x9f\x8e\xae\xf4\x8f\xbf\xbf", limits, 13}
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint16_t wide[64]; char narrow[128];
        size_t bytes = strlen(cases[i].utf8);
        memset(wide, 0xa5, sizeof(wide)); memset(narrow, 0xa5, sizeof(narrow));
        expect(utf8_to_utf16(wide, cases[i].units + 1u, cases[i].utf8) == 0,
               "valid UTF-8 fits exact output capacity");
        expect(!memcmp(wide, cases[i].utf16, (cases[i].units + 1u) * sizeof(*wide)),
               "UTF-8 gives independent expected UTF-16 units");
        expect(wide[cases[i].units + 1u] == 0xa5a5, "UTF-16 conversion preserves guard");
        expect(utf16_to_utf8(narrow, bytes + 1u, cases[i].utf16) == 0 &&
               !strcmp(narrow, cases[i].utf8), "UTF-16 gives exact original UTF-8");
        expect((unsigned char)narrow[bytes + 1u] == 0xa5, "UTF-8 conversion preserves guard");
        if (cases[i].units) {
            memset(wide, 0xa5, sizeof(wide)); memset(narrow, 0xa5, sizeof(narrow));
            expect(utf8_to_utf16(wide, cases[i].units, cases[i].utf8) == -ENAMETOOLONG &&
                   !wide[0], "short UTF-16 output fails empty, never truncated");
            expect(wide[cases[i].units] == 0xa5a5, "short UTF-16 output preserves capacity guard");
            expect(utf16_to_utf8(narrow, bytes, cases[i].utf16) == -ENAMETOOLONG &&
                   !narrow[0], "short UTF-8 output fails empty, never truncated");
            expect((unsigned char)narrow[bytes] == 0xa5, "short UTF-8 output preserves capacity guard");
        }
    }
}

static void check_invalid(void)
{
    static const char *invalid[] = {
        "\x80", "\xbf", "\xc0\xaf", "\xc1\xbf", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x80\x80\x80",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff",
        "\xc2", "\xe4\xb8", "\xf0\x9f\x8e", "\xe4x\xad", "\xc2\xc2"
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        uint16_t output[32]; char input[32];
        snprintf(input, sizeof(input), "ok/%s", invalid[i]);
        memset(output, 0xa5, sizeof(output));
        expect(utf8_to_utf16(output, 32, input) == -EILSEQ && !output[0],
               "invalid/overlong/truncated UTF-8 fails empty after valid prefix");
        expect(output[31] == 0xa5a5, "invalid UTF-8 preserves output guard");
    }
    static const uint16_t invalid16[][5] = {
        {0xd800, 0}, {0xdc00, 0}, {'a', 0xd800, 'b', 0},
        {'a', 0xd800, 0xd800, 0}, {'a', 0xdfff, 0}, {0xdbff, 0xe000, 0}
    };
    for (unsigned i = 0; i < sizeof(invalid16)/sizeof(invalid16[0]); ++i) {
        char output[32]; memset(output, 0xa5, sizeof(output));
        expect(utf16_to_utf8(output, 32, invalid16[i]) == -EILSEQ && !output[0],
               "unpaired UTF-16 surrogate fails empty");
        expect((unsigned char)output[31] == 0xa5, "invalid UTF-16 preserves output guard");
    }
    uint16_t wide[2] = {0xa5a5, 0xa5a5}; char narrow[2] = {'x', 'y'};
    expect(utf8_to_utf16(NULL, 2, "a") == -EINVAL, "null wide output");
    expect(utf16_to_utf8(NULL, 2, wide) == -EINVAL, "null narrow output");
    expect(utf8_to_utf16(wide, 0, "a") == -EINVAL && wide[0] == 0xa5a5,
           "zero wide capacity does not write");
    expect(utf16_to_utf8(narrow, 0, wide) == -EINVAL && narrow[0] == 'x',
           "zero narrow capacity does not write");
    expect(utf8_to_utf16(wide, 2, NULL) == -EINVAL && !wide[0], "null UTF-8 input empties output");
    expect(utf16_to_utf8(narrow, 2, NULL) == -EINVAL && !narrow[0], "null UTF-16 input empties output");
}

int main(void)
{
    check_valid(); check_invalid();
    if (failures) return 1;
    puts("path_encoding PASS: ASCII/CJK/BMP/surrogate pairs, exact capacities, malformed UTF-8/UTF-16 rejection, empty-on-error guards");
    return 0;
}
