#ifndef X2600_REGBA_PORT_H
#define X2600_REGBA_PORT_H
#include <time.h>
#include <unistd.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef FILE *FILE_TAG_TYPE;
typedef struct timespec timespec;
#define MAX_PATH 512
#define MAX_FILE 512
#define READONLY_CODE_CACHE_SIZE (4 * 1024 * 1024)
#define WRITABLE_CODE_CACHE_SIZE (4 * 1024 * 1024)
#define TRANSLATION_CACHE_LIMIT_THRESHOLD 1024
#define MAX_AUTO_FRAMESKIP 4
#define FILE_OPEN_APPEND "a+"
#define FILE_OPEN_READ "rb"
#define FILE_OPEN_WRITE "wb"
FILE *regba_fopen(const char *path, const char *mode);
#define FILE_OPEN(tag,path,mode) tag = regba_fopen(path, FILE_OPEN_##mode)
#define FILE_TAG_INVALID NULL
#define FILE_CHECK_VALID(tag) ((tag) != NULL)
#define FILE_CLOSE(tag) fclose(tag)
#define FILE_DELETE(path) unlink(path)
#define FILE_READ(tag,ptr,size) fread(ptr,1,size,tag)
#define FILE_WRITE(tag,ptr,size) fwrite(ptr,1,size,tag)
#define FILE_SEEK(tag,offset,type) fseek(tag,offset,type)
#define FILE_TELL(tag) ftell(tag)
#include "main.h"
void regba_frame_boundary(void);
#endif
