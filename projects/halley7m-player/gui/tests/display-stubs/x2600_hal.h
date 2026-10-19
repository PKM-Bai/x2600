/* Virtual hardware boundary for the actual production display driver. */
#ifndef DISPLAY_HOST_HAL_H
#define DISPLAY_HOST_HAL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef _MSC_VER
#define __attribute__(value)
#endif
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { unsigned long DC_INT_FLAG, DC_INTC, DC_CTRL, FRM_DES_READ;
    unsigned long LAY0_DES_READ, LAY1_DES_READ, LAY0_SITE, LAY1_SITE, DC_CLR_ST;
    unsigned long DC_FRM_CFG_ADDR, DC_FRM_CFG_CTRL, DC_COM_CONFIG, WDMA_SITE; } DPU_TypeDef;
typedef struct { unsigned long LPCLK_CTRL, PWR_UP, PHY_RSTZ, VID_MODE_CFG; } DSI_TypeDef;
typedef struct { unsigned long CLKGR0, CLKGR1, PWMCDR, LPCDR, SRBC; } CPM_TypeDef;
typedef struct { unsigned long PxPINL, PxINT, PxMSK, PxPAT1, PxPAT0; } GPIO_TypeDef;
typedef struct { unsigned long PWMEN, PWMMS, PWMINL, PWMIDL, PWMCCn[16], PWMWCn[16]; } PWM_TypeDef;
extern DPU_TypeDef fake_dpu;
extern DSI_TypeDef fake_dsi;
extern CPM_TypeDef fake_cpm;
extern GPIO_TypeDef fake_gpio;
extern PWM_TypeDef fake_pwm;
extern volatile unsigned long fake_rotator_regs[17];
#define PLAYER_ROTATOR_MMIO fake_rotator_regs
#define DPU_Instance (&fake_dpu)
#define MIPI_DSI_Instance (&fake_dsi)
#define CPM_Instance (&fake_cpm)
#define PC_Instance (&fake_gpio)
#define PWM_Instance (&fake_pwm)
#define MAX_LAYER_DESC_NUM 4
struct lcd_data { int unused; };
struct lay_cfg {
    unsigned lay_en, lay_z_order, format, color, g_alpha_en, g_alpha_val;
    unsigned source_w, source_h, stride, uv_stride, scale_w, scale_h;
    unsigned domain_multi, addr, uv_addr;
};
struct FrameDesc { uint32_t words[16]; };
struct LayerDesc { uint32_t words[16]; };
typedef struct {
    DPU_TypeDef *Instance;
    struct lcd_data *pdata;
    struct { int srd_stopped; } RdmaData;
    struct { struct FrameDesc FrameDesc[3]; struct LayerDesc LayerDesc[3][4];
        unsigned comp_next_frame; int comp_stopped; } CompData;
} LCD_HandleTypeDef;
typedef struct { unsigned PLLMux, Div, Config; } LL_CPM_CGU_ConfigTypeDef;
#define LPCDR_MPLL 0
#define ENABLE 1
#define DISABLE 0
#define LAYER_Z_ORDER_0 0
#define LAYER_Z_ORDER_1 1
#define LAYER_Z_ORDER_2 2
#define LAYER_Z_ORDER_3 3
#define LAYER_CFG_FORMAT_NV12 8
#define LAYER_CFG_FORMAT_ARGB8888 5
#define COLOR_RGB 0
#define CPM_CLKID_MIPI_DSI 0
#define CPM_CLKID_LCD 1
#define CPM_CLKID_ROTATE 48
#define QCK_STOP 0
#define IRQ_INTC0_LCD 7
#define IRQ_INTC0_ROTATE 29
#define DC_INTC_INT_UOT_0 (1u << 8)
#define DC_INTC_INT_EOD_0 (1u << 17)
#define DC_INTC_INT_EOW_0 (1u << 15)
#define DC_INTC_INT_OOW_0 (1u << 16)
#define DC_DPU_INTC_EOD_MSK_0 DC_INTC_INT_EOD_0
#define DC_DPU_INTC_UOT_MSK_0 DC_INTC_INT_UOT_0
#define DC_CTRL_DES_CNT_RST_0 (1u << 15)
#define VID_MODE_CFG_VPG_EN (1u << 16)
uint32_t fake_physical(uintptr_t pointer);
#define CPHYSADDR(p) fake_physical((uintptr_t)(p))
unsigned long fake_read(const volatile unsigned long *reg);
void fake_write(volatile unsigned long *reg, unsigned long value);
#define READ_REG(r) fake_read(&(r))
#define WRITE_REG(r,v) fake_write(&(r),(v))
#define SET_BIT(r,v) WRITE_REG(r, READ_REG(r) | (v))
#define CLEAR_BIT(r,v) WRITE_REG(r, READ_REG(r) & ~(v))
extern int fake_working;
#define __HAL_SRD_WORKING(h) ((void)(h), fake_working)
#define __HAL_WORKING(h) ((void)(h), fake_working)
HAL_StatusTypeDef HAL_LCDC_Init(LCD_HandleTypeDef *lcd);
HAL_StatusTypeDef HAL_LCDC_Enable(LCD_HandleTypeDef *lcd);
HAL_StatusTypeDef HAL_COMP_Init(LCD_HandleTypeDef *lcd, struct lay_cfg *layers);
HAL_StatusTypeDef HAL_COMP_Start(LCD_HandleTypeDef *lcd);
HAL_StatusTypeDef HAL_COMP_Stop(LCD_HandleTypeDef *lcd, int mode);
HAL_StatusTypeDef HAL_SRDMA_Stop(LCD_HandleTypeDef *lcd, int mode);
static inline HAL_StatusTypeDef CPM_CGU_LCD_Start(CPM_TypeDef *cpm, LL_CPM_CGU_ConfigTypeDef *clock)
{ (void)cpm; (void)clock; return HAL_OK; }
static inline void CPM_GATE_Enable(CPM_TypeDef *cpm, int gate) { (void)cpm; (void)gate; }
static inline void CPM_GATE_Disable(CPM_TypeDef *cpm, int gate) { (void)cpm; (void)gate; }
#endif
