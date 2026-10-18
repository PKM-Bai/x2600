/* ReGBA MIPS core diagnostic; no RTOS, drivers, flash or target-board access. */
#include "common.h"
#include "test_resources.h"
#include <setjmp.h>
#include <stdarg.h>

static uint16_t pixels[240 * 160] __attribute__((aligned(64)));
uint16_t *GBAScreen = pixels;
uint32_t GBAScreenPitch = 240;
static jmp_buf finished;
static unsigned rendered;
static int boundary_pending;
static FILE *mapped_file;
static int save_fixture_active;
static unsigned save_fixture_loads, save_fixture_submissions;
static uint8_t save_fixture[65536], save_submitted[65536];
extern uint32_t backup_update;
extern const uint32_t write_backup_delay;
extern uint32_t save_backup(void);
extern void write_backup(uint32_t address, uint32_t value);
extern uint8_t read_backup(uint32_t address);

#ifdef REGBA_DEPTH_REGRESSION
static unsigned char depth_rom[sizeof(selftest_rom)];

static void set_rom_branch(unsigned from, unsigned to)
{
    uint32_t instruction = 0xea000000u |
        (((int32_t)to - (int32_t)from - 8) / 4 & 0x00ffffffu);
    memcpy(depth_rom + from, &instruction, sizeof(instruction));
}

static void prepare_depth_rom(void)
{
    unsigned entry;
    uint32_t first;
    memcpy(depth_rom, selftest_rom, sizeof(depth_rom));
    memcpy(&first, depth_rom, sizeof(first));
    /* The diagnostic ROM starts with an ARM B instruction. Preserve its
     * original destination after a chain longer than the depth limit. */
    entry = 8 + ((int32_t)(first << 8) >> 6);
    set_rom_branch(0, 0x7000);
    for (unsigned i = 0; i < 32; ++i)
        set_rom_branch(0x7000 + i * 32,
                       i == 31 ? entry : 0x7000 + (i + 1) * 32);
}
#endif

FILE *regba_fopen(const char *name, const char *mode)
{
    if (!strcmp(name, "/__selftest.gba"))
#ifdef REGBA_DEPTH_REGRESSION
        return fmemopen(depth_rom, sizeof(depth_rom), "rb");
#else
        return fmemopen((void *)selftest_rom, sizeof(selftest_rom), "rb");
#endif
    (void)mode;
    errno = ENOENT;
    return NULL;
}
void ReGBA_Trace(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
}
/* The ISA simulator has coherent instruction/data memory; this does not test
 * the board's physical caches or the production code-publication routine. */
void ReGBA_MakeCodeVisible(void *code, size_t length) { (void)code; (void)length; }
void ReGBA_BadJump(uint32_t from, uint32_t to)
{
    printf("BAD JUMP %08lx -> %08lx\n", (unsigned long)from, (unsigned long)to);
    longjmp(finished, 2);
}
void ReGBA_MaxBlockExitsReached(uint32_t from, uint32_t to, uint32_t count)
{ (void)from; (void)to; (void)count; }
void ReGBA_MaxBlockSizeReached(uint32_t from, uint32_t to, uint32_t count)
{ (void)from; (void)to; (void)count; }
void ReGBA_RenderScreen(void)
{
    ++rendered;
    boundary_pending = 1;
    if (rendered == 1u || rendered % 30u == 0u)
        printf("rendered=%u\n", rendered);
}
bool ReGBA_IsRenderingNextFrame(void) { return true; }
void ReGBA_DisplayFPS(void) { }
void regba_frame_boundary(void)
{
    if (boundary_pending) {
        boundary_pending = 0;
        if (rendered >= 120u)
            longjmp(finished, 1);
    }
}
signed int ReGBA_AudioUpdate(void)
{ ReGBA_DiscardAudioSamples(ReGBA_GetAudioSamplesAvailable()); return 0; }
enum ReGBA_Buttons ReGBA_GetPressedButtons(void) { return 0; }
uint32_t ReGBA_Menu(enum ReGBA_MenuEntryReason reason)
{ (void)reason; longjmp(finished, 3); }
void ReGBA_LoadRTCTime(struct ReGBA_RTC *rtc)
{ *rtc = (struct ReGBA_RTC){ .year = 26, .month = 1, .day = 1, .weekday = 4 }; }
bool ReGBA_GetBackupFilename(char *out, const char *path)
{
    if (!save_fixture_active || strcmp(path, "/__selftest.gba")) return false;
    strcpy(out, "/__save_fixture64.sav");
    return true;
}
bool ReGBA_GetSavedStateFilename(char *out, const char *path, uint32_t slot)
{ (void)out; (void)path; (void)slot; return false; }
bool ReGBA_GetBundledGameConfig(char *out) { (void)out; return false; }
size_t FILE_LENGTH(FILE *file)
{
    long current = ftell(file);
    if (current < 0 || fseek(file, 0, SEEK_END)) return 0;
    long length = ftell(file);
    if (length < 0 || fseek(file, current, SEEK_SET)) return 0;
    return (size_t)length;
}
uint8_t *ReGBA_MapEntireROM(FILE *file, size_t length)
{
    size_t padded = (length + 32767u) & ~32767u;
    uint8_t *rom = malloc(padded);
    if (!rom) return NULL;
    memset(rom, 0xff, padded);
    if (fseek(file, 0, SEEK_SET) || fread(rom, 1, length, file) != length) {
        free(rom);
        return NULL;
    }
    mapped_file = file;
    return rom;
}
void ReGBA_UnmapEntireROM(void *rom)
{ if (mapped_file) fclose(mapped_file); mapped_file = NULL; free(rom); }
uint8_t *ReGBA_AllocateROM(size_t length) { return malloc(length); }
size_t ReGBA_AllocateOnDemandBuffer(void **buffer) { *buffer = NULL; return 0; }
void ReGBA_DeallocateROM(void *buffer) { free(buffer); }
ssize_t load_file_zip(const char *path, uint8_t **buffer)
{ (void)path; *buffer = NULL; errno = ENOTSUP; return -1; }
void ReGBA_OnGameLoaded(const char *path)
{ printf("loaded %s\n", path); }
void ReGBA_ProgressInitialise(enum ReGBA_FileAction action) { (void)action; }
void ReGBA_ProgressUpdate(uint32_t current, uint32_t total)
{ (void)current; (void)total; }
void ReGBA_ProgressFinalise(void) { }
bool ReGBA_SaveSettings(char *name, bool pergame)
{ (void)name; (void)pergame; return false; }
void ReGBA_LoadSettings(char *name, bool pergame) { (void)name; (void)pergame; }
void get_ticks_us(u64 *value) { *value = 0; }
void delay_us(u32 us) { (void)us; }
void synchronize(void) { }
void quit(void) { longjmp(finished, 3); }
void error_quit(void) { longjmp(finished, 4); }
extern void init_main(void);
void reset_gba(void) { init_main(); init_memory(); init_cpu(0); reset_sound(); }

/* The fixture exercises the derived core's real load/config/write/save paths;
 * no host or target storage is touched. The 120-frame diagnostic has no save.
 */
int battery_save_load(const char *path, void *data, size_t capacity, size_t *size)
{
    *size = 0;
    if (!save_fixture_active || strcmp(path, "/__save_fixture64.sav")) return -ENOENT;
    if (capacity < sizeof(save_fixture)) return -EFBIG;
    memcpy(data, save_fixture, sizeof(save_fixture));
    *size = sizeof(save_fixture);
    ++save_fixture_loads;
    return 0;
}
int battery_save_submit(const char *path, const void *data, size_t size)
{
    if (!save_fixture_active || strcmp(path, "/__save_fixture64.sav")) return -ENOSYS;
    if (size != sizeof(save_submitted)) return -EFBIG;
    memcpy(save_submitted, data, size);
    ++save_fixture_submissions;
    return 0;
}

static int save64_regression(void)
{
    int passed;
    for (unsigned i = 0; i < sizeof(save_fixture); ++i)
        save_fixture[i] = (uint8_t)(i * 13u + 7u);
    save_fixture_active = 1;
    /* load_gamepak calls the production load_backup then load_game_config.
     * No config exists, so an inferred SRAM type must survive that call.
     */
    passed = load_gamepak("/__selftest.gba") == 0 && save_fixture_loads == 1;
    passed &= !memcmp(gamepak_backup, save_fixture, sizeof(save_fixture));
    /* An unchanged loaded save can also be saved manually before any read. */
    passed &= save_backup() == 1 && save_fixture_submissions == 1;
    passed &= !memcmp(save_submitted, save_fixture, sizeof(save_fixture));

    backup_update = write_backup_delay + 1;
    write_backup(0x8123, 0x5c);
    save_fixture[0x8123] = 0x5c;
    passed &= read_backup(0x8123) == 0x5c && backup_update == write_backup_delay;
    backup_update = write_backup_delay + 1;
    write_backup(0x5555, 0x3c); /* Ordinary SRAM write, not a Flash unlock. */
    save_fixture[0x5555] = 0x3c;
    passed &= read_backup(0x5555) == 0x3c && backup_update == write_backup_delay;
    passed &= save_backup() == 1 && save_fixture_submissions == 2;
    passed &= !memcmp(save_submitted, save_fixture, sizeof(save_fixture));

    /* The same 64KiB file is ambiguous. Flash's unlock must promote the type
     * before touching its command addresses, and programming must still work.
     */
    backup_update = write_backup_delay + 1;
    write_backup(0x5555, 0xaa);
    write_backup(0x2aaa, 0x55);
    write_backup(0x5555, 0xa0);
    passed &= gamepak_backup[0x5555] == save_fixture[0x5555];
    passed &= gamepak_backup[0x2aaa] == save_fixture[0x2aaa];
    passed &= backup_update == write_backup_delay + 1;
    write_backup(0x8123, 0xea);
    save_fixture[0x8123] = 0xea;
    passed &= read_backup(0x8123) == 0xea && backup_update == write_backup_delay;
    passed &= save_backup() == 1 && save_fixture_submissions == 3;
    passed &= !memcmp(save_submitted, save_fixture, sizeof(save_fixture));
    save_fixture_active = 0;
    printf("%s: 64KiB backup load/config/manual-save/SRAM upper-half/special-address/Flash promotion/program/save-size\n",
           passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    static const uint32_t expected[] = {42, 55, 66, 77, 6, 0x600d};
    volatile int passed;
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("ReGBA MIPS software diagnostic: 120 frames");
#ifdef REGBA_DEPTH_REGRESSION
    puts("Depth regression: 32 ARM branch blocks before diagnostic ROM");
    prepare_depth_rom();
#endif
    strcpy(main_path, "/__no_config");
    memcpy(bios.rom, openbios, sizeof(openbios));
    init_sound();
    if (!save64_regression()) return 1;
    if (load_gamepak("/__selftest.gba")) {
        puts("FAIL: diagnostic ROM load");
        return 1;
    }
    int reason = setjmp(finished);
    if (!reason) {
        reg[CHANGED_PC_STATUS] = 1;
        execute_arm_translate(execute_cycles);
    }
    passed = reason == 1 && rendered == 120u;
    printf("exit reason=%d rendered=%u\n", reason, rendered);
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        uint32_t actual;
        memcpy(&actual, ewram_data + i * 4u, sizeof(actual));
        printf("mailbox[%u]=%08lx expected=%08lx\n", i,
               (unsigned long)actual, (unsigned long)expected[i]);
        passed &= actual == expected[i];
    }
    uint32_t irq_count;
    memcpy(&irq_count, ewram_data + 24u, sizeof(irq_count));
    printf("VBlank IRQ count=%lu (expected >1)\n", (unsigned long)irq_count);
    passed &= irq_count > 1u;
    for (unsigned i = 0; i < 240u * 160u; ++i)
        passed &= pixels[i] == 0x3def;
#ifdef REGBA_DEPTH_REGRESSION
    printf("translation_recursion_peak=%lu expected=%u current=%lu\n",
           (unsigned long)translation_recursion_peak,
           (unsigned)REGBA_MAX_TRANSLATION_DEPTH,
           (unsigned long)translation_recursion_level);
    passed &= translation_recursion_peak == REGBA_MAX_TRANSLATION_DEPTH;
    passed &= translation_recursion_level == 0;
#endif
    printf("%s: ARM/Thumb/SWI/IRQ/RAM-code/SMC/120 RGB555 frames\n",
           passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
