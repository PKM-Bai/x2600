/**
 * @file x2600_sysclk_conf.h
 * @author MPU系统软件部团队
 * @brief
 *
 * @copyright 版权所有 (北京君正集成电路股份有限公司) {2022}
 * @copyright Copyright© 2022 Ingenic Semiconductor Co.,Ltd
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __X2600_SYSCLK_CONF_H__
#define __X2600_SYSCLK_CONF_H__

#ifdef __cplusplus
extern "C" {
#endif
/* 1. Includes ---------------------------------------------------- */

/* 2. Exported Types ---------------------------------------------- */


/* 3. Exported Constants ------------------------------------------ */

/* 4. Exported Macros --------------------------------------------- */
#define SYSCLK_EXTAL        (24000000)
#define SYSCLK_APLL     (1152000000)
#define SYSCLK_MPLL     (1800000000)

#define SystemCoreClock SYSCLK_APLL

#define CGU_CONFIG_MSC_APLL_24M       {         \
         .PLLMux = MSC1CDR_SCLK_A,              \
         .Div = 24,                             \
         .Config = 0 }

#define CGU_CONFIG_MSC_APLL_48M       {         \
         .PLLMux = MSC1CDR_SCLK_A,              \
         .Div = 11,                             \
         .Config = 0 }


#define CGU_CONFIG_SSI_MPLL_500K      {         \
         .PLLMux = SSICDR_MPLL,                 \
         .Div = 15,                             \
         .Config = 0 }

/* 5. Exported Funcs ---------------------------------------------- */

/* 6. Exported Variables ------------------------------------------ */

/* 7. Private Types ----------------------------------------------- */

/* 8. Private Constants ------------------------------------------- */

/* 9. Private Macros ---------------------------------------------- */

/* 10. Private Funcs ---------------------------------------------- */

/* 11. Private Variables ------------------------------------------ */

#ifdef __cplusplus
}
#endif
#endif /* __X2600_HAL_ADC_H__ */
