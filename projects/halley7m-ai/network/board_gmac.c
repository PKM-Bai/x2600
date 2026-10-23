/* X2600-only management-interface probe. Register facts are cross-checked
 * against X2600 PM chapters 20/36 and U-Boot x2600m_halley7.h.
 * RMII mode is deliberately preserved: PM says 4, this U-Boot config says 2.
 * Resolving that discrepancy is required before MAC frame traffic. */
#include "board_gmac.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <errno.h>
#include <string.h>

#define MAC_REGISTER(offset) (*(volatile uint32_t *)(UINT32_C(0xb34b0000) + (offset)))
#define MAC_GATE (UINT32_C(1) << 6)
#define CLOCK_CHANGE (UINT32_C(1) << 29)
#define CLOCK_BUSY (UINT32_C(1) << 28)
#define CLOCK_STOP (UINT32_C(1) << 27)
#define MDIO_BUSY UINT32_C(1)
#define MDIO_CLOCK_DIV_124 (UINT32_C(5) << 2)

static void delay_us(uint32_t delay)
{
    uint64_t until = platform_time_us() + delay;
    while (platform_time_us() < until) { }
}

static uint32_t pll_rate(uint32_t control)
{
    unsigned m = (control >> 20) & 0xfff;
    unsigned n = (control >> 14) & 0x3f;
    unsigned od1 = (control >> 11) & 7;
    unsigned od0 = (control >> 8) & 7;
    if (!m || !n || !od1 || !od0 || !(control & 1))
        return 0;
    return (uint32_t)(UINT64_C(24000000) * m / (n * od1 * od0));
}

static uint32_t bus_rate(uint32_t cpccr, unsigned mux_shift, unsigned div_shift,
                         uint32_t apll, uint32_t mpll)
{
    unsigned mux = (cpccr >> mux_shift) & 3;
    uint32_t source = mux == 1 ? apll : mux == 2 ? mpll : 0;
    return source / (((cpccr >> div_shift) & 15) + 1);
}

static int clock_write(uint32_t value)
{
    uint64_t deadline = platform_time_us() + 10000;
    while (CPM_Instance->MACPHYCDR & CLOCK_BUSY)
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
    CPM_Instance->MACPHYCDR = (value & ~CLOCK_BUSY) | CLOCK_CHANGE;
    while (CPM_Instance->MACPHYCDR & CLOCK_BUSY)
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
    CPM_Instance->MACPHYCDR = value & ~(CLOCK_BUSY | CLOCK_CHANGE);
    return 0;
}

static int mdio_idle(void)
{
    uint64_t deadline = platform_time_us() + 10000;
    while (MAC_REGISTER(0x10) & MDIO_BUSY)
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
    return 0;
}

static int mdio_read(unsigned phy, unsigned reg, uint16_t *value)
{
    int result = mdio_idle();
    if (result)
        return result;
    MAC_REGISTER(0x10) = ((uint32_t)phy << 11) | ((uint32_t)reg << 6) |
                          MDIO_CLOCK_DIV_124 | MDIO_BUSY;
    result = mdio_idle();
    if (!result)
        *value = (uint16_t)MAC_REGISTER(0x14);
    return result;
}

int ai_board_gmac_probe(ai_gmac_probe_t *report)
{
    if (!report)
        return -EINVAL;
    memset(report, 0, sizeof(*report));
    uint32_t apll = pll_rate(CPM_Instance->CPAPCR);
    uint32_t mpll = pll_rate(CPM_Instance->CPMPCR);
    uint32_t cpccr = CPM_Instance->CPCCR;
    report->ahb0_hz = bus_rate(cpccr, 26, 8, apll, mpll);
    report->ahb2_hz = bus_rate(cpccr, 24, 12, apll, mpll);
    /* X2600 PM table 4-9 (pp.67-68) places MAC 0x134b0000 on AHB2.
     * Section 36.5 derives MDC from MAC HCLK. H0 belongs to other devices;
     * using H0 here incorrectly rejects the measured 360MHz/300MHz profile.
     * Integer H2<=300MHz and CSR /124 keep MDC below 2.5MHz. */
    if (!report->ahb2_hz || report->ahb2_hz > 300000000 ||
        !mpll || mpll % 50000000 || mpll / 50000000 > 256 ||
        CPM_Instance->CPMPACR != 0 || CPM_Instance->CPAPACR != 0)
        return -ERANGE;
    unsigned long flags = _local_irq_save();
    uint32_t old_gate = CPM_Instance->CLKGR0 & MAC_GATE;
    CPM_Instance->CLKGR0 &= ~MAC_GATE;
    (void)CPM_Instance->CLKGR0;
    local_irq_restore(flags);
    int result = 0;
    if ((MAC_REGISTER(0) & 0x0c) || (MAC_REGISTER(0x1018) & 0x2002)) {
        result = -EBUSY;
        goto restore_gate;
    }
    uint32_t old_clock = CPM_Instance->MACPHYCDR;
    report->phy_mode = CPM_Instance->MACPHYC0;
    report->mac_version = MAC_REGISTER(0x20);
    uint32_t old_pins[10];
    for (unsigned i = 0; i < 10; ++i)
        old_pins[i] = LL_GPIO_getPinMode(PC_Instance, 15 + i);
    result = clock_write(old_clock | CLOCK_STOP);
    if (result)
        goto restore_clock;
    result = clock_write((UINT32_C(1) << 30) | (mpll / 50000000 - 1));
    if (result)
        goto restore_clock;
    report->mac_clock = CPM_Instance->MACPHYCDR;
    LL_GPIO_setPinMaskMode(PC_Instance, UINT32_C(0x3ff) << 15, GPIO_MODE_INPUT);
    LL_GPIO_setPinMode(PC_Instance, 15, GPIO_MODE_OUTPUT0);
    delay_us(10000);
    LL_GPIO_setPinMode(PC_Instance, 15, GPIO_MODE_OUTPUT1);
    delay_us(10000);
    LL_GPIO_setPinMaskMode(PC_Instance, UINT32_C(0x3ff) << 15, GPIO_MODE_FUNCTION0);
    delay_us(10);
    for (unsigned phy = 0; phy < 32; ++phy) {
        result = mdio_read(phy, 2, &report->id1[phy]);
        if (result)
            break;
        result = mdio_read(phy, 3, &report->id2[phy]);
        if (result)
            break;
        uint32_t id = ((uint32_t)report->id1[phy] << 16) | report->id2[phy];
        if (id == 0 || id == UINT32_MAX)
            continue;
        report->found_mask |= UINT32_C(1) << phy;
        /* BMSR link is latch-low; read twice to report the current state. */
        result = mdio_read(phy, 1, &report->status[phy]);
        if (!result)
            result = mdio_read(phy, 1, &report->status[phy]);
        if (result)
            break;
    }
    for (unsigned i = 0; i < 10; ++i)
        LL_GPIO_setPinMode(PC_Instance, 15 + i, old_pins[i]);
    if (!result && !report->found_mask)
        result = -ENODEV;
restore_clock:
    if (clock_write(old_clock) != 0)
        result = -ETIMEDOUT;
restore_gate:
    flags = _local_irq_save();
    if (old_gate)
        CPM_Instance->CLKGR0 |= MAC_GATE;
    local_irq_restore(flags);
    return result;
}
