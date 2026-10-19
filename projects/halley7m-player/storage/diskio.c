#include "diskio.h"
#include "sdspi.h"

DSTATUS disk_initialize(BYTE drive)
{
    return drive || sdspi_initialize() < 0 ? STA_NOINIT : STA_PROTECT;
}

DSTATUS disk_status(BYTE drive)
{
    return drive || !sdspi_is_initialized() ? STA_NOINIT : STA_PROTECT;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, DWORD sector, UINT count)
{
    if (drive || !buffer || !count) return RES_PARERR;
    if (!sdspi_is_initialized()) return RES_NOTRDY;
    return sdspi_read(buffer, sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (drive) return RES_PARERR;
    if (!sdspi_is_initialized()) return RES_NOTRDY;
    if (command == CTRL_SYNC) return RES_OK; /* No writes can be queued. */
    if (!buffer) return RES_PARERR;
    switch (command) {
    case GET_SECTOR_COUNT: *(DWORD *)buffer = sdspi_sector_count(); return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buffer = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buffer = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
