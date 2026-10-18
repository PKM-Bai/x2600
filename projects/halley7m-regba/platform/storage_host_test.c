/* Native regression only. Build with SDSPI_HOST_TEST; never link in firmware.
 * Uses a synthetic FAT32 volume in RAM, never a host disk or physical TF card. */
#include "sdspi.h"
#include "storage.h"
#include "path_encoding.h"
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SECTORS 131072u
#define FAT_SECTORS 1024u
#define PARTITION_START 2048u
#define DISK_SECTORS (TEST_SECTORS + PARTITION_START)
static BYTE *ram_disk;

/* Test setup and readback enter the actual FatFs UTF-16 API through the same
 * production converter as storage. No L literals or native wchar_t casts. */
static void path16(TCHAR output[513], const char *input)
{
    assert(sizeof(TCHAR) == sizeof(uint16_t));
    assert(utf8_to_utf16(output, 513, input) == 0);
}
static FRESULT fat_mount(FATFS *fs, BYTE immediate)
{ static const TCHAR root[] = {0}; return f_mount(fs, root, immediate); }
static FRESULT fat_open(FIL *file, const char *path, BYTE mode)
{ TCHAR wide[513]; path16(wide, path); return f_open(file, wide, mode); }
static FRESULT fat_mkdir(const char *path)
{ TCHAR wide[513]; path16(wide, path); return f_mkdir(wide); }
static FRESULT fat_stat(const char *path, FILINFO *info)
{ TCHAR wide[513]; path16(wide, path); return f_stat(wide, info); }
static FRESULT fat_unlink(const char *path)
{ TCHAR wide[513]; path16(wide, path); return f_unlink(wide); }
static FRESULT fat_rename(const char *old_path, const char *new_path)
{
    TCHAR old_wide[513], new_wide[513];
    path16(old_wide, old_path); path16(new_wide, new_path);
    return f_rename(old_wide, new_wide);
}

static void write_bytes(const char *path, const void *bytes, UINT size)
{
    FIL file; UINT moved;
    assert(fat_open(&file, path, FA_WRITE | FA_CREATE_NEW) == FR_OK);
    assert(f_write(&file, bytes, size, &moved) == FR_OK && moved == size);
    assert(f_sync(&file) == FR_OK);
    assert(f_close(&file) == FR_OK);
}

static void read_bytes(const char *path, const void *expected, UINT size)
{
    FIL file; UINT moved; uint8_t data[128];
    assert(size <= sizeof(data));
    assert(fat_open(&file, path, FA_READ) == FR_OK && f_size(&file) == size);
    assert(f_read(&file, data, sizeof(data), &moved) == FR_OK && moved == size);
    assert(!memcmp(data, expected, size));
    assert(f_close(&file) == FR_OK);
}

DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return drive ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE drive, BYTE *data, DWORD sector, UINT count)
{
    if (drive || !count || sector >= DISK_SECTORS || count > DISK_SECTORS - sector)
        return RES_PARERR;
    memcpy(data, ram_disk + (size_t)sector * 512, (size_t)count * 512);
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *data, DWORD sector, UINT count)
{
    if (drive || !count || sector >= DISK_SECTORS || count > DISK_SECTORS - sector)
        return RES_PARERR;
    memcpy(ram_disk + (size_t)sector * 512, data, (size_t)count * 512);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *data)
{
    if (drive) return RES_PARERR;
    if (command == CTRL_SYNC) return RES_OK;
    if (command == GET_SECTOR_COUNT) { *(DWORD *)data = DISK_SECTORS; return RES_OK; }
    if (command == GET_SECTOR_SIZE) { *(WORD *)data = 512; return RES_OK; }
    if (command == GET_BLOCK_SIZE) { *(DWORD *)data = 1; return RES_OK; }
    return RES_PARERR;
}

static void le16(BYTE *p, unsigned value) { p[0] = value; p[1] = value >> 8; }
static void le32(BYTE *p, uint32_t value)
{ le16(p, value); le16(p + 2, value >> 16); }

static void make_ram_volume(void)
{
    ram_disk = calloc(DISK_SECTORS, 512);
    assert(ram_disk);
    BYTE *b = ram_disk;
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "REGBA   ", 8);
    le16(b + 11, 512); b[13] = 1; le16(b + 14, 32); b[16] = 2;
    b[21] = 0xf8; le32(b + 32, TEST_SECTORS); le32(b + 36, FAT_SECTORS);
    le32(b + 44, 2); le16(b + 48, 1); le16(b + 50, 6);
    b[64] = 0x80; b[66] = 0x29; le32(b + 67, 0x12345678);
    memcpy(b + 71, "REGBA TEST ", 11); memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xaa;
    memcpy(b + 6 * 512, b, 512);
    b += 512;
    le32(b, 0x41615252); le32(b + 484, 0x61417272);
    le32(b + 488, 0xffffffff); le32(b + 492, 3);
    b[510] = 0x55; b[511] = 0xaa;
    for (unsigned i = 0; i < 2; i++) {
        b = ram_disk + (size_t)(32 + i * FAT_SECTORS) * 512;
        le32(b, 0x0ffffff8); le32(b + 4, 0x0fffffff); le32(b + 8, 0x0fffffff);
    }
}

static void check_sd_math(void)
{
    const uint8_t cmd0[] = { 0x40, 0, 0, 0, 0 };
    const uint8_t cmd8[] = { 0x48, 0, 0, 1, 0xaa };
    assert(sdspi_crc7(cmd0, sizeof(cmd0)) == 0x95);
    assert(sdspi_crc7(cmd8, sizeof(cmd8)) == 0x87);
    assert(sdspi_crc16((const uint8_t *)"123456789", 9) == 0x31c3);
    uint8_t csd[16] = {0};
    uint32_t sectors;
    csd[0] = 0x40;
    csd[8] = csd[9] = 0xff;
    assert(sdspi_decode_csd(csd, &sectors) == 0 && sectors == 67108864u);
    csd[7] = 0x3f;
    assert(sdspi_decode_csd(csd, &sectors) == -EOVERFLOW);
    memset(csd, 0, sizeof(csd));
    csd[5] = 9; csd[6] = 3; csd[7] = 0xff; csd[8] = 0xc0;
    csd[9] = 3; csd[10] = 0x80;
    assert(sdspi_decode_csd(csd, &sectors) == 0 && sectors == 2097152u);
    csd[0] = 0x80;
    assert(sdspi_decode_csd(csd, &sectors) == -ENOTSUP);
    for (unsigned value = 0; value < 256; value++)
        assert(ff_convert(ff_convert((WCHAR)value, 1), 0) == value);
    assert(ff_wtoupper('z') == 'Z');
    printf("PASS: SD CRC7/CRC16, CSD v1/v2/overflow, CP437 conversion\n");
}

static void check_unicode(FATFS *fs)
{
    static const char directory[] = u8"/中文游戏";
    static const char nested[] = u8"/中文游戏/口袋妖怪";
    static const char rom[] = u8"精灵宝可梦 绿宝石.gba";
    static const char rom_bytes[] = "CJK-ROM\0binary-content";
    storage_entry_t entries[4]; uint32_t total; size_t count;
    char path[600]; FILINFO info;
    assert(fat_mkdir(directory) == FR_OK && fat_mkdir(nested) == FR_OK);
    snprintf(path, sizeof(path), "%s/%s", nested, rom);
    write_bytes(path, rom_bytes, sizeof(rom_bytes));
    write_bytes(u8"/中文游戏/超级玛丽.GBA", "MARIO", 5);
    write_bytes(u8"/中文游戏/说明.txt", "not a ROM", 9);
    assert(storage_scan_gba(directory, 0, entries, 4, &total, &count) == 0);
    assert(total == 2 && count == 2);
    assert(entries[0].directory && !strcmp(entries[0].name, u8"口袋妖怪"));
    assert(!entries[1].directory && !strcmp(entries[1].name, u8"超级玛丽.GBA"));
    snprintf(path, sizeof(path), "%s/%s", directory, entries[1].name);
    read_bytes(path, "MARIO", 5);
    snprintf(path, sizeof(path), "%s/%s", directory, entries[0].name);
    assert(storage_scan_gba(path, 0, entries, 4, &total, &count) == 0);
    assert(total == 1 && count == 1 && !entries[0].directory &&
           !strcmp(entries[0].name, rom) && entries[0].size == sizeof(rom_bytes));
    snprintf(path, sizeof(path), "%s/%s", nested, entries[0].name);
    read_bytes(path, rom_bytes, sizeof(rom_bytes));

    /* The battery commit keeps the previous save until the new temporary
     * file is synced and renamed. Check both independent Unicode operands. */
    static const char save[] = u8"/GBA/SAVES/精灵宝可梦 绿宝石.sav";
    static const char temp[] = u8"/GBA/SAVES/精灵宝可梦 绿宝石.sav.tmp";
    static const char backup[] = u8"/GBA/SAVES/精灵宝可梦 绿宝石.sav.bak";
    assert(fat_mkdir("/GBA/SAVES") == FR_OK);
    write_bytes(save, "old-save", 8); write_bytes(temp, "new-save-content", 16);
    assert(fat_rename(save, backup) == FR_OK && fat_rename(temp, save) == FR_OK);
    assert(fat_mount(NULL, 0) == FR_OK && fat_mount(fs, 1) == FR_OK);
    read_bytes(save, "new-save-content", 16); read_bytes(backup, "old-save", 8);
    assert(fat_stat(temp, &info) == FR_NO_FILE);
    assert(fat_unlink(backup) == FR_OK && fat_stat(backup, &info) == FR_NO_FILE);

    /* FatFs permits 255 UTF-16 units, while application names have a separate
     * 255-byte UTF-8 budget. A valid boundary name must survive unchanged. */
    char name[260];
    assert(fat_mkdir(u8"/长文件名") == FR_OK);
    for (unsigned i = 0; i < 83; ++i) memcpy(name + i * 3u, u8"汉", 3);
    memcpy(name + 249, "xy.gba", 7);
    assert(strlen(name) == 255);
    snprintf(path, sizeof(path), u8"/长文件名/%s", name);
    write_bytes(path, "boundary", 8);
    assert(storage_scan_gba(u8"/长文件名", 0, entries, 4, &total, &count) == 0);
    assert(total == 1 && count == 1 && !strcmp(entries[0].name, name));
    snprintf(path, sizeof(path), u8"/长文件名/%s", entries[0].name);
    read_bytes(path, "boundary", 8);
    for (unsigned i = 0; i < 84; ++i) memcpy(name + i * 3u, u8"汉", 3);
    memcpy(name + 252, ".gba", 5);
    assert(strlen(name) == 256);
    snprintf(path, sizeof(path), u8"/长文件名/%s", name);
    write_bytes(path, "too long", 8);
    total = 77; count = 77;
    assert(storage_scan_gba(u8"/长文件名", 0, entries, 4, &total, &count) == -ENAMETOOLONG);
    assert(!total && !count);
    assert(storage_scan_gba(u8"/长文件名", 0, NULL, 0, &total, &count) == -ENAMETOOLONG);
    assert(fat_unlink(path) == FR_OK);
    assert(storage_scan_gba(u8"/长文件名", 0, entries, 4, &total, &count) == 0 && count == 1);

    /* Total UTF-8 path length is 512 bytes independently of name length. */
    memcpy(path, u8"/边界", sizeof(u8"/边界")); assert(strlen(path) == 7);
    assert(fat_mkdir(path) == FR_OK);
    for (unsigned level = 0; level < 3; ++level) {
        size_t used = strlen(path); path[used++] = '/';
        memset(path + used, 'a' + (int)level, 160); path[used + 160] = 0;
        assert(fat_mkdir(path) == FR_OK);
    }
    assert(strlen(path) == 490);
    path[490] = '/'; memset(path + 491, 'z', 17); memcpy(path + 508, ".gba", 5);
    assert(strlen(path) == 512); write_bytes(path, "path512", 7); read_bytes(path, "path512", 7);
    memset(path + 491, 'd', 21); path[512] = 0;
    assert(fat_mkdir(path) == FR_OK);
    assert(storage_scan_gba(path, 0, entries, 4, &total, &count) == 0 && !total && !count);
    path[512] = 'x'; path[513] = 0; total = 77; count = 77;
    assert(storage_scan_gba(path, 0, entries, 4, &total, &count) == -ENAMETOOLONG && !total && !count);
    total = 77; count = 77;
    assert(storage_scan_gba("/bad\xc0\xaf", 0, entries, 4, &total, &count) == -EILSEQ && !total && !count);
    puts("PASS: real FatFs UTF-16 LFN / UTF-8 scan and reopen, CJK directories/save rename/remount/unlink, 255-byte names and 512-byte paths, no truncation/invalid UTF-8 rejection");
}

static void check_fatfs(void)
{
    FATFS fs;
    FIL file;
    UINT moved;
    uint8_t data[4096], readback[4096];
    make_ram_volume();
    assert(fat_mount(&fs, 1) == FR_OK && fs.fs_type == FS_FAT32);
    assert(fat_mkdir("/GBA") == FR_OK);
    assert(fat_mkdir("/GBA/TEST") == FR_OK);
    const char *path = "/GBA/TEST/long filename 128KiB.bin";
    assert(fat_open(&file, path, FA_WRITE | FA_CREATE_NEW) == FR_OK);
    for (uint32_t offset = 0; offset < 131072; offset += sizeof(data)) {
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)((offset + i) * 37u ^ ((offset + i) >> 8));
        assert(f_write(&file, data, sizeof(data), &moved) == FR_OK && moved == sizeof(data));
    }
    assert(f_sync(&file) == FR_OK);
    assert(f_close(&file) == FR_OK);
    assert(fat_mount(NULL, 0) == FR_OK);
    assert(fat_mount(&fs, 1) == FR_OK);
    assert(fat_open(&file, "/gba/test/LONG FILENAME 128KIB.BIN", FA_READ) == FR_OK);
    assert(f_size(&file) == 131072);
    for (uint32_t offset = 0; offset < 131072; offset += sizeof(readback)) {
        assert(f_read(&file, readback, sizeof(readback), &moved) == FR_OK && moved == sizeof(readback));
        for (unsigned i = 0; i < sizeof(readback); i++)
            assert(readback[i] == (uint8_t)((offset + i) * 37u ^ ((offset + i) >> 8)));
    }
    assert(f_read(&file, readback, sizeof(readback), &moved) == FR_OK && moved == 0);
    assert(f_lseek(&file, 131000) == FR_OK);
    assert(f_read(&file, readback, sizeof(readback), &moved) == FR_OK && moved == 72);
    assert(f_close(&file) == FR_OK);
    assert(fat_open(&file, path, FA_CREATE_NEW | FA_WRITE) == FR_EXIST);
    assert(fat_rename(path, "/GBA/TEST/renamed.bin") == FR_OK);
    FILINFO info;
    assert(fat_stat("/GBA/TEST/renamed.bin", &info) == FR_OK && info.fsize == 131072);
    assert(fat_unlink("/GBA/TEST/renamed.bin") == FR_OK);
    assert(fat_stat("/GBA/TEST/renamed.bin", &info) == FR_NO_FILE);
    assert(fat_mkdir("/GBA/ROMS") == FR_OK);
    assert(fat_mkdir("/GBA/ROMS/RPG") == FR_OK);
    const char *roms[] = {"First.gba", "Second.GBA", "notes.txt", "hidden.gba", "system.gba"};
    for (unsigned i = 0; i < 5; ++i) {
        char rom_path[80];
        snprintf(rom_path, sizeof(rom_path), "/GBA/ROMS/%s", roms[i]);
        assert(fat_open(&file, rom_path, FA_WRITE | FA_CREATE_NEW) == FR_OK);
        /* Mark the freshly created synthetic entry directly: firmware omits
         * FatFs chmod, so tests do not enable an unused production feature. */
        if (i >= 3) {
            file.dir_ptr[11] |= i == 3 ? AM_HID : AM_SYS;
            file.obj.fs->wflag = 1;
        }
        assert(f_write(&file, "GBA", 3, &moved) == FR_OK && moved == 3);
        assert(f_close(&file) == FR_OK);
    }
    storage_entry_t page[2];
    uint32_t total;
    size_t count;
    assert(storage_scan_gba("/GBA/ROMS", 0, page, 2, &total, &count) == 0);
    assert(total == 3 && count == 2);
    assert(page[0].directory && !strcmp(page[0].name, "RPG"));
    assert(!page[1].directory && !strcmp(page[1].name, "First.gba") && page[1].size == 3);
    assert(storage_scan_gba("/GBA/ROMS", 2, page, 2, &total, &count) == 0);
    assert(total == 3 && count == 1 && !strcmp(page[0].name, "Second.GBA"));
    assert(storage_scan_gba("/GBA/ROMS", 99, page, 2, &total, &count) == 0 && total == 3 && !count);
    assert(storage_scan_gba("/GBA/ROMS", 0, NULL, 0, &total, &count) == 0 && total == 3 && !count);
    assert(storage_scan_gba("/GBA/ROMS/RPG", 0, page, 2, &total, &count) == 0 && !total && !count);
    assert(storage_scan_gba("/missing", 0, page, 2, &total, &count) == -ENOENT && !total && !count);
    assert(storage_scan_gba("/GBA/ROMS", 0, NULL, 1, &total, &count) == -EINVAL);
    check_unicode(&fs);
    assert(fat_mount(NULL, 0) == FR_OK);
    /* Real TF cards usually contain an MBR. Check partition-offset handling
     * against the same volume after moving it away from sector zero. */
    memmove(ram_disk + (size_t)PARTITION_START * 512, ram_disk,
            (size_t)TEST_SECTORS * 512);
    memset(ram_disk, 0, (size_t)PARTITION_START * 512);
    ram_disk[446 + 4] = 0x0c;
    le32(ram_disk + 446 + 8, PARTITION_START);
    le32(ram_disk + 446 + 12, TEST_SECTORS);
    ram_disk[510] = 0x55; ram_disk[511] = 0xaa;
    assert(fat_mount(&fs, 1) == FR_OK && fs.volbase == PARTITION_START);
    assert(fat_open(&file, "/GBA/TEST/partition.bin", FA_WRITE | FA_CREATE_NEW) == FR_OK);
    memset(data, 0xa5, sizeof(data));
    assert(f_write(&file, data, sizeof(data), &moved) == FR_OK && moved == sizeof(data));
    assert(f_close(&file) == FR_OK);
    assert(fat_mount(NULL, 0) == FR_OK);
    assert(fat_mount(&fs, 1) == FR_OK && fs.volbase == PARTITION_START);
    assert(fat_open(&file, "/GBA/TEST/partition.bin", FA_READ) == FR_OK);
    assert(f_read(&file, readback, sizeof(readback), &moved) == FR_OK && moved == sizeof(readback));
    assert(memcmp(data, readback, sizeof(data)) == 0);
    assert(f_close(&file) == FR_OK);
    assert(fat_mount(NULL, 0) == FR_OK);
    free(ram_disk);
    printf("PASS: FAT32 RAM volume + MBR, LFN, 128KiB sync/remount, ROM filtering/pagination/directories/errors\n");
}

int main(void)
{
    check_sd_math();
    check_fatfs();
    return 0;
}
