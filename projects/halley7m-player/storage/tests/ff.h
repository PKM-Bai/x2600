#ifndef PLAYER_STORAGE_TEST_FF_H
#define PLAYER_STORAGE_TEST_FF_H
#include <stdint.h>
#define _FS_READONLY 1
#define _FS_EXFAT 0
#define _FS_REENTRANT 0
#define _USE_FASTSEEK 1
#define CREATE_LINKMAP UINT32_MAX
#define FA_READ 1
#define AM_DIR 0x10
typedef uint16_t TCHAR;
typedef uint32_t FSIZE_t;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef struct { int mounted; } FATFS;
typedef struct { struct { FSIZE_t objsize; } obj; FSIZE_t fptr; DWORD *cltbl; } FIL;
typedef struct { unsigned index; } DIR;
typedef struct { FSIZE_t fsize; uint8_t fattrib; TCHAR fname[256]; } FILINFO;
typedef enum {
    FR_OK, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT, FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM, FR_MKFS_ABORTED,
    FR_TIMEOUT, FR_LOCKED, FR_NOT_ENOUGH_CORE, FR_TOO_MANY_OPEN_FILES, FR_INVALID_PARAMETER
} FRESULT;
#define f_size(fp) ((fp)->obj.objsize)
#define f_tell(fp) ((fp)->fptr)
FRESULT f_mount(FATFS *fs, const TCHAR *path, uint8_t opt);
FRESULT f_open(FIL *fp, const TCHAR *path, uint8_t mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buffer, UINT bytes, UINT *received);
FRESULT f_lseek(FIL *fp, FSIZE_t offset);
FRESULT f_opendir(DIR *directory, const TCHAR *path);
FRESULT f_readdir(DIR *directory, FILINFO *entry);
FRESULT f_closedir(DIR *directory);
#endif
