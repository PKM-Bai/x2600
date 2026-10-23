#ifndef AI_LWIP_ARCH_CC_H
#define AI_LWIP_ARCH_CC_H

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include "ai_lwip_platform.h"

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX /* XBurst2 newlib ssize_t is signed long. */
#endif
#define LWIP_TIMEVAL_PRIVATE 0
#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_PLATFORM_DIAG(message) do { printf message; } while (0)
#define LWIP_PLATFORM_ASSERT(message) ai_lwip_assert(message, __FILE__, __LINE__)
#define LWIP_RAND() ai_lwip_random()
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(field) field
typedef unsigned long sys_prot_t;

#endif
