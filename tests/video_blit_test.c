/* Host regression for the production portrait blitter. GPL-2.0-or-later. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "video_blit.h"
#include "ui_draw.h"

enum {
    SOURCE_WIDTH = 240,
    SOURCE_HEIGHT = 160,
    SOURCE_STRIDE = 256,
    TARGET_WIDTH = 720,
    TARGET_HEIGHT = 1280,
    TARGET_STRIDE = 736,
    GUARD_WORDS = 64,
    SOURCE_WORDS = GUARD_WORDS + SOURCE_HEIGHT * SOURCE_STRIDE + GUARD_WORDS,
    TARGET_WORDS = GUARD_WORDS + TARGET_HEIGHT * TARGET_STRIDE + GUARD_WORDS
};

struct color_case {
    uint16_t gba;
    uint32_t xrgb;
};

/* Independent literal expectations, including intermediate grey levels that
 * distinguish 5-to-8-bit replication from a plain left shift or truncation. */
static const struct color_case colors[] = {
    {0x0000, 0xff000000u}, /* black */
    {0x001f, 0xffff0000u}, /* red occupies the low five GBA bits */
    {0x03e0, 0xff00ff00u},
    {0x7c00, 0xff0000ffu},
    {0x7fff, 0xffffffffu},
    {0x0010, 0xff840000u},
    {0x0200, 0xff008400u},
    {0x4000, 0xff000084u},
    {0x0421, 0xff080808u},
    {0x0842, 0xff101010u},
    {0x1084, 0xff212121u},
    {0x2108, 0xff424242u},
    {0x3def, 0xff7b7b7bu},
    {0x4210, 0xff848484u},
    {0x03ff, 0xffffff00u},
    {0x7c1f, 0xffff00ffu},
    {0x7fe0, 0xff00ffffu}
};

static uint16_t source_storage[SOURCE_WORDS];
static uint16_t source_snapshot[SOURCE_WORDS];
static uint32_t target_storage[TARGET_WORDS];
static uint32_t hud_snapshot[720u * 64u];

static size_t color_at(unsigned x, unsigned y)
{
    /* An asymmetric 2D pattern catches row/column swaps and stride mistakes.
     * Explicit corner markers make placement errors easy to localize. */
    if (x == 0 && y == 0) return 1;
    if (x == SOURCE_WIDTH - 1 && y == 0) return 2;
    if (x == 0 && y == SOURCE_HEIGHT - 1) return 3;
    if (x == SOURCE_WIDTH - 1 && y == SOURCE_HEIGHT - 1) return 4;
    return (x * 7u + y * 11u + (x ^ y)) % (sizeof(colors) / sizeof(colors[0]));
}

static uint32_t target_canary(size_t index)
{
    /* Blitter output has alpha FF; guards deliberately have a different alpha. */
    return 0x5a000000u | (((uint32_t)index * 2654435761u) & 0x00ffffffu);
}

static unsigned run_case(unsigned high_bit_flip)
{
    size_t color_counts[sizeof(colors) / sizeof(colors[0])][2] = {{0}};
    unsigned failures = 0;
    uint16_t *source = source_storage + GUARD_WORDS;
    uint32_t *target = target_storage + GUARD_WORDS;

    for (size_t i = 0; i < SOURCE_WORDS; ++i)
        source_storage[i] = (uint16_t)(0xa55au ^ (uint16_t)i);
    for (size_t i = 0; i < TARGET_WORDS; ++i)
        target_storage[i] = target_canary(i);
    for (unsigned y = 0; y < SOURCE_HEIGHT; ++y) {
        for (unsigned x = 0; x < SOURCE_WIDTH; ++x) {
            size_t index = color_at(x, y);
            unsigned bit15 = ((x + y) & 1u) ^ high_bit_flip;
            source[y * SOURCE_STRIDE + x] =
                (uint16_t)(colors[index].gba | (bit15 ? 0x8000u : 0));
            ++color_counts[index][bit15];
        }
    }
    memcpy(source_snapshot, source_storage, sizeof(source_storage));

    video_blit_gba(target, TARGET_STRIDE, source, SOURCE_STRIDE);

    /* Check the whole allocation, including borders, all 16 padding pixels per
     * row, and guards. Inverse coordinate mapping is deliberately separate from
     * the production writer's forward loops; colors come only from literals. */
    for (size_t i = 0; i < TARGET_WORDS; ++i) {
        uint32_t expected = target_canary(i);
        if (i >= GUARD_WORDS && i < TARGET_WORDS - GUARD_WORDS) {
            size_t image_index = i - GUARD_WORDS;
            unsigned y = (unsigned)(image_index / TARGET_STRIDE);
            unsigned x = (unsigned)(image_index % TARGET_STRIDE);
            if (x < TARGET_WIDTH && y >= 400 && y < 880)
                expected = colors[color_at(x / 3u, (y - 400u) / 3u)].xrgb;
        }
        if (target_storage[i] != expected) {
            if (failures < 8)
                fprintf(stderr, "video pass=%u word=%zu expected=%08x got=%08x\n",
                        high_bit_flip, i, (unsigned)expected,
                        (unsigned)target_storage[i]);
            ++failures;
        }
    }
    for (size_t i = 0; i < SOURCE_WORDS; ++i) {
        if (source_storage[i] != source_snapshot[i]) {
            if (failures < 8)
                fprintf(stderr, "video pass=%u changed source word=%zu\n",
                        high_bit_flip, i);
            ++failures;
        }
    }
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        if (!color_counts[i][0] || !color_counts[i][1]) {
            fprintf(stderr, "video pattern missed color=%zu or its bit15 variant\n", i);
            ++failures;
        }
    }
    return failures;
}

static unsigned run_hud(unsigned width, unsigned height, const game_stats_t *stats,
                        const char *image)
{
    unsigned failures = 0, bright = 0;
    uint32_t *target = target_storage + GUARD_WORDS;
    size_t frame_words = (size_t)width * height;
    for (size_t i = 0; i < TARGET_WORDS; ++i) target_storage[i] = target_canary(i);
    int full_game = width == 720 && height == 1280;
    if (full_game)
        video_blit_gba(target, width, source_storage + GUARD_WORDS, SOURCE_STRIDE);
    ui_draw_game_stats(target, width, height, stats);
    for (size_t i = 0; i < TARGET_WORDS; ++i) {
        uint32_t expected = target_canary(i);
        if (i >= GUARD_WORDS && i - GUARD_WORDS < frame_words) {
            size_t offset = i - GUARD_WORDS;
            unsigned x = (unsigned)(offset % width), y = (unsigned)(offset / width);
            if (width && height >= 384 && stats && y >= 320 && y < 384) {
                if ((target_storage[i] >> 24) != 0xff) ++failures;
                if (target_storage[i] != 0xff000000u) ++bright;
                continue;
            }
            if (full_game && y >= 400 && y < 880)
                expected = colors[color_at(x / 3u, (y - 400u) / 3u)].xrgb;
        }
        if (target_storage[i] != expected) {
            if (failures < 4)
                fprintf(stderr, "HUD outside strip width=%u height=%u word=%zu\n", width, height, i);
            ++failures;
        }
    }
    if (full_game && !bright) { fputs("HUD text is absent\n", stderr); ++failures; }
    if (image) {
        FILE *file = fopen(image, "wb");
        if (!file) { perror(image); return failures + 1; }
        if (fwrite(target, sizeof(*target), frame_words, file) != frame_words) ++failures;
        if (fclose(file)) ++failures;
    }
    return failures;
}

int main(int argc, char **argv)
{
    unsigned failures = run_case(0) + run_case(1);
    game_stats_t stats = { .cpu_tenths=750, .fps_tenths=597, .valid=1, .running=1 };
    char image[512];
    if (argc > 1) snprintf(image, sizeof(image), "%s/hud-values.bgra", argv[1]);
    failures += run_hud(720, 1280, &stats, argc > 1 ? image : NULL);
    memcpy(hud_snapshot, target_storage + GUARD_WORDS + 320u * 720u, sizeof(hud_snapshot));
    stats.valid = 0;
    if (argc > 1) snprintf(image, sizeof(image), "%s/hud-pending.bgra", argv[1]);
    failures += run_hud(720, 1280, &stats, argc > 1 ? image : NULL);
    if (!memcmp(hud_snapshot, target_storage + GUARD_WORDS + 320u * 720u, sizeof(hud_snapshot))) {
        fputs("HUD placeholder must differ from valid values\n", stderr); ++failures;
    }
    failures += run_hud(32, 384, &stats, NULL);
    failures += run_hud(720, 383, &stats, NULL);
    failures += run_hud(720, 384, &stats, NULL);
    failures += run_hud(0, 384, &stats, NULL);
    if (failures) {
        fprintf(stderr, "video_blit FAILED: %u mismatches\n", failures);
        return 1;
    }
    puts("video_blit PASS: RGB/greys/bit15, 3x pixels, padded strides/guards; HUD values/placeholders stay in rows320..383 and preserve game pixels");
    return 0;
}
