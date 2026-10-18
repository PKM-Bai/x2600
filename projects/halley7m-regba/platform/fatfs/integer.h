/**
 *@file integer.h
 *@author MPU系统软件部团队
 *@brief Fatfs头文件
 *
 *@copyright 版权所有 (北京君正集成电路股份有限公司) {2024}
 *@copyright Copyright© 2024 Ingenic Semiconductor Co.,Ltd
 *
 *This program is free software; you can redistribute  it and/or modify it
 *under  the terms of  the GNU General  Public License as published by the
 *Free Software Foundation;  either version 2 of the  License, or (at your
 *option) any later version.
 **/

#ifndef _FF_INTEGER
#define _FF_INTEGER

#if 0 /* SDK Windows typedefs are not used by this embedded/native-test port. */

#include <windows.h>
#include <tchar.h>

/** @addtogroup FatFs
  * @{
  */

/* 导出的类型 (Exported Types) ----------------------------------------------- */
	 /** @defgroup FatFs_Exported_Types FatFs 导出类型 (Exported Types)
	  *   * @{
	  *     */

typedef unsigned __int64 QWORD;


#else			/* Embedded platform */

/* These types MUST be 16-bit or 32-bit */
typedef int				INT;
typedef unsigned int	UINT;

/* This type MUST be 8-bit */
typedef unsigned char	BYTE;

/* These types MUST be 16-bit */
typedef short			SHORT;
typedef unsigned short	WORD;
typedef unsigned short	WCHAR;

/* These types MUST be 32-bit */
#include <stdint.h>
typedef int32_t         LONG;
typedef uint32_t        DWORD;

/* This type MUST be 64-bit (Remove this for ANSI C (C89) compatibility) */
typedef unsigned long long QWORD;
/**
 *   * @}
 *     */

#endif

#endif
/**
 *   * @}
 *     */
/**********************************END OF FILE**********************************/
