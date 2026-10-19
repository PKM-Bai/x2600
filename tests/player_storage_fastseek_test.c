/* Production adapter + derived FatFs against a virtual FAT32 block device.
 * Only boot/root/FAT sectors are synthesized; no multi-GiB host file exists. */
#include "player_storage.h"
#include "ff.h"
#include "diskio.h"
#include "FreeRTOS.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLUSTER_BYTES 32768u
#define CLUSTER_SECTORS 64u
#define CLUSTERS 340000u
#define FAT_START 32u
#define FAT_SECTORS ((CLUSTERS + 2u + 127u) / 128u)
#define DATA_START (FAT_START + FAT_SECTORS)
#define SECTORS (DATA_START + CLUSTERS * CLUSTER_SECTORS)
#define FILE_BYTES UINT32_C(2147591940)
#define FILE_CLUSTERS ((FILE_BYTES + CLUSTER_BYTES - 1u) / CLUSTER_BYTES)
#define FRAG_A 70000u
#define FRAG_B 140000u
#define FIRST_FRAGMENT 32768u
#define FULL_START 200000u
#define SMALL_START 333000u

static uint64_t now_us;
static uint32_t fat_reads, data_reads, last_data_sector, pumps, delay_calls, cancel_at;
int xPortIsInsideInterrupt(void) { return 0; }
int xTaskGetSchedulerState(void) { return taskSCHEDULER_RUNNING; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)(uintptr_t)1; }
uint64_t platform_time_us(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { assert(ticks == 1); ++delay_calls; now_us += 1000; }
int sdspi_gpio_probe(void) { assert(0); return -1; }
static int pump(void *opaque)
{
    assert(opaque == &pumps);
    ++pumps;
    return cancel_at && pumps >= cancel_at ? -ECANCELED : 0;
}
static void le16(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }
static void le32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i)); }

static uint32_t fat_entry(uint32_t cluster)
{
    if (!cluster) return 0x0ffffff8u;
    if (cluster == 1 || cluster == 2 || cluster == SMALL_START) return 0x0fffffffu;
    if (cluster >= 3 && cluster < 3 + FILE_CLUSTERS)
        return cluster + 1u == 3 + FILE_CLUSTERS ? 0x0fffffffu : cluster + 1u;
    if (cluster >= FRAG_A && cluster < FRAG_A + FIRST_FRAGMENT)
        return cluster + 1u == FRAG_A + FIRST_FRAGMENT ? FRAG_B : cluster + 1u;
    if (cluster >= FRAG_B && cluster < FRAG_B + FILE_CLUSTERS - FIRST_FRAGMENT)
        return cluster + 1u == FRAG_B + FILE_CLUSTERS - FIRST_FRAGMENT ? 0x0fffffffu : cluster + 1u;
    if (cluster >= FULL_START && !(cluster & 1u) && (cluster - FULL_START) / 2u < FILE_CLUSTERS)
        return (cluster - FULL_START) / 2u + 1u == FILE_CLUSTERS ? 0x0fffffffu : cluster + 2u;
    return 0;
}

static void directory_entry(uint8_t *p, const char *name, uint32_t cluster, uint32_t bytes)
{
    memcpy(p, name, 11);
    p[11] = 0x20;
    le16(p + 20, (uint16_t)(cluster >> 16));
    le16(p + 26, (uint16_t)cluster);
    le32(p + 28, bytes);
}

DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NOINIT : STA_PROTECT; }
DSTATUS disk_status(BYTE drive) { return drive ? STA_NOINIT : STA_PROTECT; }
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive) return RES_PARERR;
    if (command == CTRL_SYNC) return RES_OK;
    if (!buffer) return RES_PARERR;
    if (command == GET_SECTOR_COUNT) *(DWORD *)buffer = SECTORS;
    else if (command == GET_SECTOR_SIZE) *(WORD *)buffer = 512;
    else if (command == GET_BLOCK_SIZE) *(DWORD *)buffer = 1;
    else return RES_PARERR;
    return RES_OK;
}
DRESULT disk_read(BYTE drive, BYTE *buffer, DWORD sector, UINT count)
{
    if (drive || !buffer || !count || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    for (UINT block = 0; block < count; ++block, ++sector, buffer += 512) {
        now_us += 100;
        memset(buffer, 0, 512);
        if (!sector) {
            buffer[0] = 0xeb; buffer[1] = 0x58; buffer[2] = 0x90;
            memcpy(buffer + 3, "MSWIN4.1", 8);
            le16(buffer + 11, 512); buffer[13] = CLUSTER_SECTORS;
            le16(buffer + 14, FAT_START); buffer[16] = 1; buffer[21] = 0xf8;
            le32(buffer + 32, SECTORS); le32(buffer + 36, FAT_SECTORS);
            le32(buffer + 44, 2); le16(buffer + 48, 1); le16(buffer + 50, 6);
            buffer[66] = 0x29; memcpy(buffer + 82, "FAT32   ", 8);
            buffer[510] = 0x55; buffer[511] = 0xaa;
        } else if (sector >= FAT_START && sector < DATA_START) {
            ++fat_reads;
            for (unsigned entry = 0; entry < 128; ++entry)
                le32(buffer + entry * 4u, fat_entry((sector - FAT_START) * 128u + entry));
        } else if (sector == DATA_START) {
            directory_entry(buffer, "CONTIG  AVI", 3, FILE_BYTES);
            directory_entry(buffer + 32, "FRAG    AVI", FRAG_A, FILE_BYTES);
            directory_entry(buffer + 64, "FULL    AVI", FULL_START, FILE_BYTES);
            directory_entry(buffer + 96, "SMALL   WAV", SMALL_START, 16);
        } else if (sector >= DATA_START) {
            ++data_reads;
            last_data_sector = sector;
            for (unsigned i = 0; i < 512; ++i) buffer[i] = (uint8_t)(sector ^ (sector >> 8) ^ i);
        }
    }
    return RES_OK;
}

static void check_mapped_file(const char *path, uint32_t expected_entries, int fragmented)
{
    struct player_storage_file *file;
    player_storage_seek_stats stats;
    assert(player_storage_open_utf8(&file, path) == 0);
    pumps = delay_calls = cancel_at = 0;
    assert(player_storage_prepare_seek(file, pump, &pumps) == 0);
    assert(player_storage_get_seek_stats(file, &stats) == 0);
    assert(stats.enabled && stats.capacity_entries == 4096 && stats.used_entries == expected_entries);
    assert(stats.required_entries == expected_entries && stats.build_us > 0);
    assert(delay_calls == FILE_CLUSTERS / 256u && pumps == delay_calls + 2u);
    const struct player_media_io *io = player_storage_io(file);
    assert(io && io->size(io->opaque) == FILE_BYTES);
    unsigned before = fat_reads;
    uint64_t target = UINT64_C(0x80000000) + 16u;
    assert(io->seek(io->opaque, target) == 0);
    uint8_t bytes[16];
    assert(io->read(io->opaque, bytes, sizeof(bytes)) == sizeof(bytes));
    uint32_t logical = (uint32_t)(target / CLUSTER_BYTES);
    uint32_t physical = fragmented && logical >= FIRST_FRAGMENT ? FRAG_B + logical - FIRST_FRAGMENT :
                        (fragmented ? FRAG_A : 3u) + logical;
    uint32_t expected_sector = DATA_START + (physical - 2u) * CLUSTER_SECTORS;
    assert(last_data_sector == expected_sector);
    assert(bytes[0] == (uint8_t)(expected_sector ^ (expected_sector >> 8) ^ 16u));
    assert(io->seek(io->opaque, 48) == 0); /* Also exercise a backward fast seek. */
    assert(fat_reads == before); /* Neither direction follows FAT after loading. */
    assert(player_storage_get_seek_stats(file, &stats) == 0 && stats.seek_calls == 2 && stats.max_seek_us == 100);
    unsigned saved_pumps = pumps;
    assert(player_storage_prepare_seek(file, pump, &pumps) == 0 && pumps == saved_pumps);
    assert(player_storage_close(file) == 0);
    assert(player_storage_get_seek_stats(NULL, &stats) == 0 && stats.used_entries == expected_entries);
}

int main(void)
{
    assert(player_storage_mount() == 0);
    check_mapped_file("0:/CONTIG.AVI", 4, 0);
    check_mapped_file("0:/FRAG.AVI", 6, 1);
    struct player_storage_file *file;
    player_storage_seek_stats stats;
    assert(player_storage_open_utf8(&file, "0:/FULL.AVI") == 0);
    pumps = delay_calls = cancel_at = 0;
    assert(player_storage_prepare_seek(file, pump, &pumps) == -E2BIG);
    assert(player_storage_get_seek_stats(file, &stats) == 0 && !stats.enabled);
    assert(stats.capacity_entries == 4096 && stats.required_entries == FILE_CLUSTERS * 2u + 2u);
    assert(pumps == FILE_CLUSTERS / 256u + 2u); /* Fragment boundaries cannot suppress pumping. */
    assert(player_storage_close(file) == 0);
    assert(player_storage_open_utf8(&file, "0:/CONTIG.AVI") == 0);
    pumps = delay_calls = 0; cancel_at = 3;
    assert(player_storage_prepare_seek(file, pump, &pumps) == -ECANCELED);
    assert(pumps == 3 && delay_calls == 1);
    assert(player_storage_get_seek_stats(file, &stats) == 0 && !stats.enabled);
    assert(player_storage_close(file) == 0);
    assert(player_storage_open_utf8(&file, "0:/CONTIG.AVI") == 0);
    pumps = delay_calls = 0; cancel_at = 1;
    assert(player_storage_prepare_seek(file, pump, &pumps) == -ECANCELED);
    assert(pumps == 1 && !delay_calls);
    assert(player_storage_get_seek_stats(file, &stats) == 0 && !stats.required_entries && !stats.enabled);
    assert(player_storage_close(file) == 0);
    assert(player_storage_open_utf8(&file, "0:/SMALL.WAV") == 0);
    assert(player_storage_prepare_seek(file, NULL, NULL) == 0);
    assert(player_storage_get_seek_stats(file, &stats) == 0 && !stats.enabled && !stats.capacity_entries);
    assert(player_storage_close(file) == 0);
    static const uint8_t embedded[] = {1, 2, 3};
    assert(player_storage_register_embedded(embedded, sizeof(embedded)) == 0);
    assert(player_storage_open_utf8(&file, "@embedded") == 0);
    assert(player_storage_prepare_seek(file, NULL, NULL) == 0);
    assert(player_storage_close(file) == 0);
    printf("PASS real FatFs FAT32 CLMT: >2GiB forward/back seeks without FAT I/O, fragments, bounded map/full-chain pump, cancel, small/embedded bypass (%lu data sectors)\n", (unsigned long)data_reads);
    return 0;
}
