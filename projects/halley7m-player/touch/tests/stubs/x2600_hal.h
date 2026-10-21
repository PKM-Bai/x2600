#ifndef TOUCH_TEST_X2600_HAL_H
#define TOUCH_TEST_X2600_HAL_H
#include <stdint.h>
typedef struct {
    uint32_t I2C_CON, I2C_TAR, I2C_DC, I2C_SHCNT, I2C_SLCNT;
    uint32_t I2C_SDASU, I2C_SDAHD, I2C_FSPKLEN, I2C_INTM, I2C_DMACR;
    uint32_t I2C_RXTL, I2C_TXTL, I2C_CINT, I2C_ENABLE, I2C_ENBST;
    uint32_t I2C_RINTST, I2C_ST, I2C_ABTSRC;
} I2C_TypeDef;
typedef struct { uint32_t CPCCR, CPAPCR, CPMPCR; } touch_test_cpm_t;
typedef struct { uint32_t unused; } touch_test_gpio_t;
extern I2C_TypeDef touch_test_i2c;
extern touch_test_cpm_t touch_test_cpm;
extern touch_test_gpio_t touch_test_gpio;
#define I2C0_Instance (&touch_test_i2c)
#define CPM_Instance (&touch_test_cpm)
#define PC_Instance (&touch_test_gpio)
#define SYSCLK_EXTAL 24000000u
#define CPCCR_SEL_H2PLL 0x03000000u
#define CPCCR_SEL_H2PLL_Pos 24u
#define CPCCR_PDIV 0x000f0000u
#define CPCCR_PDIV_Pos 16u
#define CPM_CLKID_I2C0 8u
#define GPIO_MODE_FUNCTION1 1u
#define GPIO_MODE_OUTPUT0 8u
#define GPIO_MODE_OUTPUT1 9u
#define GPIO_MODE_INPUT 10u
#define I2C_CON_SLVDIS (1u << 6)
#define I2C_CON_RESTART (1u << 5)
#define I2C_CON_SPEED_0 (1u << 1)
#define I2C_CON_MD 1u
#define I2C_ST_MSTACT (1u << 5)
#define I2C_ST_RFNE (1u << 3)
#define I2C_ST_TFNF (1u << 1)
#define I2C_DC_CMD (1u << 8)
#define I2C_DC_STOP (1u << 9)
#define I2C_DC_RESTART (1u << 10)
#define I2C_RAW_INTM_STAT_TX_ABRT (1u << 6)
#define I2C_RAW_INTM_STAT_STOP_DET (1u << 9)
static inline void CPM_GATE_Enable(touch_test_cpm_t *instance, unsigned id)
{ (void)instance; (void)id; }
static inline void LL_GPIO_setPinMode(touch_test_gpio_t *instance, unsigned pin, unsigned mode)
{ (void)instance; (void)pin; (void)mode; }
#endif
