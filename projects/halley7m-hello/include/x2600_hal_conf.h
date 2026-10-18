#ifndef __X2600_HAL_CONF_H__
#define __X2600_HAL_CONF_H__
/* TODO： 本文件应该通过工具生成，在配置工程中选择不同的组件时，在此处包含不同的头文件
    暂时包含全部头文件.
 */


/* 1. Includes ---------------------------------------------------- */


/* Hal Module selections. */
#if 0
    #define HAL_MSC_ENABLED
    #define HAL_I2C_ENABLED
    #define HAL_UART_ENABLED
    #define HAL_ADC_ENABLED
    #define HAL_SPI_ENABLED
    #define HAL_WDT_ENABLED
    #define HAL_TCU_ENABLED
    #define HAL_RTC_ENABLED
    #define HAL_EFUSE_ENABLED
    #define HAL_PWM_ENABLED
    #define HAL_GMAC_ENABLED
    #define HAL_USB_ENABLED
#endif

/* 系统时钟配配置，通过工具生成，随开发板或者平台变化. */
#include <x2600_sysclk_conf.h>
#include <board_eth_phy_conf.h>

#include "x2600_hal_tick.h"
#include "x2600_ll_ost_core.h"
#include "x2600_ll_ost_global.h"
#include "x2600_ll_cpm.h"
#include "x2600_ll_gpio.h"

#include "x2600_hal_pdma.h"

//#include "x2600_hal_sfcnor.h"

#ifdef HAL_MSC_ENABLED

#endif

#ifdef HAL_I2C_ENABLED
    #include "x2600_hal_i2c.h"
#endif

#ifdef HAL_UART_ENABLED
    #include "x2600_hal_uart.h"
#endif

#ifdef HAL_ADC_ENABLED
    #include "x2600_hal_sadc.h"
#endif

#ifdef HAL_SPI_ENABLED
    #include "x2600_hal_spi.h"
#endif

#ifdef HAL_WDT_ENABLED
    #include "x2600_hal_wdt.h"
#endif

#ifdef HAL_TCU_ENABLED
    #include "x2600_hal_tcu.h"
#endif

#ifdef HAL_RTC_ENABLED
    #include "x2600_hal_rtc.h"
#endif

#ifdef HAL_EFUSE_ENABLED
    #include "x2600_ll_efuse.h"
    #include "x2600_hal_efuse.h"
#endif

#ifdef HAL_PWM_ENABLED
    #include "x2600_hal_pwm.h"
#endif

#ifdef HAL_GMAC_ENABLED
    #include "x2600_hal_gmac.h"
#endif

#ifdef HAL_USB_ENABLED
    #include "x2600_hal_pcd.h"
    #include "x2600_hal_pcd_ex.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 2. Exported Types ---------------------------------------------- */

/* 3. Exported Constants ------------------------------------------ */

/* 4. Exported Macros --------------------------------------------- */

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
#endif /* __X2600_HAL_H__ */
