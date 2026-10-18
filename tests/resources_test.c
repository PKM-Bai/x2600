/* Independent path/CRC fixtures; no filesystem or board access. */
#include "resources.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;

static void expect_path(const char *rom, const char *expected)
{
    char actual[REGBA_RESOURCE_PATH_MAX + 1u];
    int result = regba_backup_path(actual, sizeof(actual), rom);
    if (result != 0 || strcmp(actual, expected)) {
        printf("FAIL backup path rom=%s result=%d got=%s expected=%s\n",
               rom, result, actual, expected);
        ++failures;
    }
}

static void expect_result(const char *name, int actual, int expected)
{
    if (actual != expected) {
        printf("FAIL %s got=%d expected=%d\n", name, actual, expected);
        ++failures;
    }
}

static void check_capacity(void)
{
    static const char expected[] = "/GBA/SAVES/game.sav";
    struct {
        unsigned char before;
        char text[sizeof(expected)];
        unsigned char after;
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    expect_result("exact capacity", regba_backup_path(guarded.text, sizeof(expected),
                  "/GBA/ROMS/game.gba"), 0);
    if (strcmp(guarded.text, expected) || guarded.before != 0xa5 || guarded.after != 0xa5)
        ++failures;
    memset(&guarded, 0xa5, sizeof(guarded));
    expect_result("one byte short", regba_backup_path(guarded.text, sizeof(expected) - 1u,
                  "/GBA/ROMS/game.gba"), -ENAMETOOLONG);
    if (guarded.text[0] != 0 || (unsigned char)guarded.text[1] != 0xa5 ||
        guarded.before != 0xa5 || guarded.after != 0xa5) ++failures;
    guarded.text[0] = 'x';
    expect_result("zero capacity", regba_backup_path(guarded.text, 0,
                  "/GBA/ROMS/game.gba"), -ENAMETOOLONG);
    if (guarded.text[0] != 'x') ++failures;
}

static void check_case_aliases(void)
{
    char lower[REGBA_RESOURCE_PATH_MAX + 1u], upper[REGBA_RESOURCE_PATH_MAX + 1u];
    size_t i;
    expect_result("lower case path", regba_backup_path(lower, sizeof(lower),
                  "/gba/roms/rpg/same.gba"), 0);
    expect_result("upper case path", regba_backup_path(upper, sizeof(upper),
                  "/GBA/ROMS/RPG/SAME.GBA"), 0);
    for (i = 0; upper[i]; ++i)
        if (upper[i] >= 'A' && upper[i] <= 'Z') upper[i] += 'a' - 'A';
    for (i = 0; lower[i]; ++i)
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 'a' - 'A';
    if (strcmp(lower, upper)) {
        puts("FAIL case variants must address the same FatFs save");
        ++failures;
    }
}

static void check_bad_paths(void)
{
    char out[REGBA_RESOURCE_PATH_MAX + 32u];
    char too_long[REGBA_RESOURCE_PATH_MAX + 2u];
    char long_stem[REGBA_RESOURCE_PATH_MAX + 1u];
    expect_result("null output", regba_backup_path(NULL, sizeof(out), "a.gba"), -EINVAL);
    expect_result("null ROM", regba_backup_path(out, sizeof(out), NULL), -EINVAL);
    expect_result("empty ROM", regba_backup_path(out, sizeof(out), ""), -EINVAL);
    expect_result("directory only", regba_backup_path(out, sizeof(out), "/GBA/ROMS/"), -EINVAL);
    expect_result("empty stem", regba_backup_path(out, sizeof(out), "/GBA/ROMS/.gba"), -EINVAL);
    expect_result("core self-test", regba_backup_path(out, sizeof(out), "/__selftest.gba"), -ENOTSUP);
    memset(too_long, 'a', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1u] = 0;
    expect_result("oversized ROM path", regba_backup_path(out, sizeof(out), too_long), -ENAMETOOLONG);
    memcpy(long_stem, "/GBA/ROMS/", 10u);
    memset(long_stem + 10u, 'a', 498u);
    memcpy(long_stem + 508u, ".gba", 5u);
    expect_result("save path exceeds platform limit", regba_backup_path(out, sizeof(out), long_stem),
                  -ENAMETOOLONG);
}

static void check_fatfs_name_limit(void)
{
    char path[256], out[REGBA_RESOURCE_PATH_MAX + 1u];
    memcpy(path, "/Other/", 7u);
    memset(path + 7u, 'a', 242u);
    memcpy(path + 249u, ".gba", 5u);
    expect_result("255-byte hashed save name", regba_backup_path(out, sizeof(out), path), 0);
    if (strlen(out) != strlen(REGBA_SAVE_DIR) + 1u + 255u) ++failures;
    path[249] = 'a';
    memcpy(path + 250u, ".gba", 5u);
    expect_result("256-byte hashed save name rejected", regba_backup_path(out, sizeof(out), path),
                  -ENAMETOOLONG);
}

int main(void)
{
    expect_path("/GBA/ROMS/game.gba", "/GBA/SAVES/game.sav");
    expect_path("/gba_game/game.gba", "/GBA/SAVES/game.sav");
    expect_path("/gBa/rOmS/Old.Name.GBA", "/GBA/SAVES/Old.Name.sav");
    expect_path("/GBA_GAME/Old.Name.GBA", "/GBA/SAVES/Old.Name.sav");
    expect_path("/GBA/ROMS/RPG/Same.gba", "/GBA/SAVES/Same-6de1a93c.sav");
    expect_path("/GBA/ROMS/Action/Same.gba", "/GBA/SAVES/Same-be990d2f.sav");
    expect_path("/Other/Adventure.v1.gba", "/GBA/SAVES/Adventure.v1-5871b3d4.sav");
    expect_path("relative.gba", "/GBA/SAVES/relative-c8724f99.sav");
    expect_path("/__save_sram.gba", "/GBA/SAVES/__save_sram.sav");
    expect_path("/__save_flash.gba", "/GBA/SAVES/__save_flash.sav");
    expect_path("/__save_eeprom.gba", "/GBA/SAVES/__save_eeprom.sav");
    check_capacity();
    check_case_aliases();
    check_bad_paths();
    check_fatfs_name_limit();
    if (failures) {
        printf("resources regression: %u failure(s)\n", failures);
        return 1;
    }
    puts("resources regression PASS: roots, compatibility, path CRC, case aliases, bounds, diagnostics");
    return 0;
}
