/* X2600-only management probe and polling frame driver. Register facts are cross-checked
 * against X2600 PM chapters 20/36 and U-Boot x2600m_halley7.h.
 * RMII mode is deliberately preserved: PM says 4, this U-Boot config says 2.
 * The driver preserves the measured mode=2; DMA frames need board validation. */
#include "board_gmac.h"
#include "platform_runtime.h"
#include <x2600_hal.h>
#include <cache.h>
#include <errno.h>
#include <string.h>

#define MAC_REGISTER(offset) (*(volatile uint32_t *)(UINT32_C(0xb34b0000) + (offset)))
#define MAC_GATE (UINT32_C(1) << 6)
#define CLOCK_CHANGE (UINT32_C(1) << 29)
#define CLOCK_BUSY (UINT32_C(1) << 28)
#define CLOCK_STOP (UINT32_C(1) << 27)
#define MDIO_BUSY UINT32_C(1)
#define MDIO_CLOCK_DIV_124 (UINT32_C(5) << 2)

static bool hardware_owned;

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
    if (hardware_owned)
        return -EBUSY;
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

/* X2600 PM 36.6 and U-Boot jz_gmac_v12/SynopGMAC_Dev enhanced descriptor.
 * Unlike U-Boot's 40-byte struct, there are no software words: ATDS=1, DSL=0. */
#define RX_COUNT 8U
#define DMA_BUFFER_SIZE 2048U
#define DESC_OWN UINT32_C(0x80000000)
#define DESC_ERROR UINT32_C(0x00008000)
#define RX_FIRST_LAST UINT32_C(0x00000300)
#define RX_END_RING UINT32_C(0x00008000)
#define TX_FIRST_LAST_END UINT32_C(0x30200000)
#define DMA_START UINT32_C(0x00002002)
#define DMA_FATAL UINT32_C(0x00002000)
#define DMA_W1C UINT32_C(0x0001e7ff)
#define DMA_STORE_FORWARD UINT32_C(0x02200000)
#define DMA_BUS_CONFIGURATION UINT32_C(0x02012080)
#define MAC_ENABLE UINT32_C(0x0000000c)

typedef struct {
    uint32_t status;
    uint32_t length;
    uint32_t buffer1;
    uint32_t buffer2;
    uint32_t extended;
    uint32_t reserved;
    uint32_t timestamp_low;
    uint32_t timestamp_high;
} dma_descriptor_t;

typedef struct __attribute__((aligned(128))) {
    dma_descriptor_t rx[RX_COUNT];
    dma_descriptor_t tx;
    unsigned char tx_buffer[DMA_BUFFER_SIZE];
    unsigned char rx_buffer[RX_COUNT][DMA_BUFFER_SIZE];
} dma_memory_t;

_Static_assert(sizeof(dma_descriptor_t) == 32, "GMAC enhanced descriptor stride");
_Static_assert(sizeof(dma_memory_t) % 128 == 0, "No cached state shares DMA tail lines");
/* Entire object isolated on L1/L2 lines; cached alias is never used after the
 * initial writeback/invalidate. Only the KSEG1 alias below touches its data. */
static dma_memory_t dma_memory __attribute__((aligned(128)));
static dma_memory_t *dma;
static ai_gmac_status_t driver;
static uint8_t station_address[6];
static uint32_t saved_clock;
static uint32_t saved_gate;
static uint32_t saved_pins[10];
static unsigned next_rx;
static bool tx_pending;
static uint64_t tx_deadline;
static bool dma_fault;
static ai_gmac_mmc_t mmc_previous;

static void mmc_counter(unsigned offset, uint32_t *previous, uint32_t *total, bool read_clear)
{
    uint32_t value = MAC_REGISTER(offset);
    *total += read_clear ? value : value - *previous;
    *previous = value;
}

static void sample_mmc(void)
{
    /* X2600 PM 36.6.2.12: HW_FEATURE bit11 is MMCSEL. Offsets and ROR
     * semantics: U-Boot SynopGMAC_Dev.h MMC_TX/MMC_RX/MMC_CNTRL enums.
     * Never change freeze/reset/ROR controls to take a diagnostic sample. */
    driver.mmc.available = (driver.hardware_features & UINT32_C(0x800)) != 0;
    if (!driver.mmc.available) return;
    driver.mmc.control = MAC_REGISTER(0x100);
    bool read_clear = (driver.mmc.control & UINT32_C(4)) != 0;
#define SAMPLE_MMC(member, offset) \
    mmc_counter(offset, &mmc_previous.member, &driver.mmc.member, read_clear)
    SAMPLE_MMC(tx_octets_gb, 0x114);
    SAMPLE_MMC(tx_frames_gb, 0x118);
    SAMPLE_MMC(tx_1024_max, 0x138);
    SAMPLE_MMC(tx_underflow, 0x148);
    SAMPLE_MMC(tx_frames_good, 0x168);
    SAMPLE_MMC(rx_frames_gb, 0x180);
    SAMPLE_MMC(rx_octets_gb, 0x184);
    SAMPLE_MMC(rx_octets_good, 0x188);
    SAMPLE_MMC(rx_crc, 0x194);
    SAMPLE_MMC(rx_align, 0x198);
    SAMPLE_MMC(rx_runt, 0x19c);
    SAMPLE_MMC(rx_jabber, 0x1a0);
    SAMPLE_MMC(rx_oversize_good, 0x1a8);
    SAMPLE_MMC(rx_1024_max, 0x1c0);
    SAMPLE_MMC(rx_overflow, 0x1d4);
    SAMPLE_MMC(rx_watchdog, 0x1dc);
#undef SAMPLE_MMC
}

static void dma_sync(void)
{
    __asm__ volatile("sync" ::: "memory");
}

static uint32_t physical(const void *address)
{
    return (uint32_t)(uintptr_t)address & UINT32_C(0x1fffffff);
}

static int driver_fail(int error)
{
    driver.last_error = error;
    dma_fault = true;
    driver.link_up = false;
    return error;
}

static int mdio_write(unsigned phy, unsigned reg, uint16_t value)
{
    int result = mdio_idle();
    if (result)
        return result;
    MAC_REGISTER(0x14) = value;
    MAC_REGISTER(0x10) = ((uint32_t)phy << 11) | ((uint32_t)reg << 6) |
                          MDIO_CLOCK_DIV_124 | UINT32_C(3);
    return mdio_idle();
}

static int controller_reset(void)
{
    /* SWR resets DMA and MAC and completes only with all interface clocks.
     * Do not repurpose buffers or release pins until SWR actually clears. */
    if (driver.initialized) sample_mmc();
    MAC_REGISTER(0x101c) = 0;
    MAC_REGISTER(0) &= ~MAC_ENABLE;
    MAC_REGISTER(0x1018) &= ~DMA_START;
    dma_sync();
    MAC_REGISTER(0x1000) = 1;
    uint64_t deadline = platform_time_us() + 100000;
    while (MAC_REGISTER(0x1000) & 1U)
        if (platform_time_us() >= deadline)
            return -ETIMEDOUT;
    dma_sync();
    /* SWR resets all MAC registers, including MMC. Keep software totals but
     * restart raw baselines, so hardware zero is not interpreted as wrap. */
    memset(&mmc_previous, 0, sizeof(mmc_previous));
    if (tx_pending) {
        ++driver.tx_dropped;
        tx_pending = false;
    }
    return 0;
}

static void configure_dma(bool link, unsigned speed, bool duplex)
{
    memset(dma, 0, sizeof(*dma));
    next_rx = 0;
    for (unsigned i = 0; i < RX_COUNT; ++i) {
        dma->rx[i].length = DMA_BUFFER_SIZE | (i == RX_COUNT - 1 ? RX_END_RING : 0);
        dma->rx[i].buffer1 = physical(dma->rx_buffer[i]);
    }
    dma->tx.status = UINT32_C(0x00200000);
    dma->tx.buffer1 = physical(dma->tx_buffer);
    dma_sync();
    for (unsigned i = 0; i < RX_COUNT; ++i)
        ((volatile dma_descriptor_t *)&dma->rx[i])->status = DESC_OWN;
    dma_sync();
    MAC_REGISTER(0x1000) = DMA_BUS_CONFIGURATION;
    MAC_REGISTER(0x100c) = physical(dma->rx);
    MAC_REGISTER(0x1010) = physical(&dma->tx);
    MAC_REGISTER(0x101c) = 0; /* Polling: no DMA interrupts. */
    MAC_REGISTER(0x3c) = 1;  /* Mask RGMII/SMII status interrupt as in v12. */
    MAC_REGISTER(0x10c) = UINT32_MAX; /* MMC RX interrupt mask. */
    MAC_REGISTER(0x110) = UINT32_MAX; /* MMC TX interrupt mask. */
    MAC_REGISTER(0x200) = UINT32_MAX; /* MMC IPC RX interrupt mask. */
    MAC_REGISTER(0x40) = UINT32_C(0x80000000) |
        ((uint32_t)station_address[5] << 8) | station_address[4];
    MAC_REGISTER(0x44) = ((uint32_t)station_address[3] << 24) |
        ((uint32_t)station_address[2] << 16) |
        ((uint32_t)station_address[1] << 8) | station_address[0];
    MAC_REGISTER(4) = 0; /* Perfect unicast plus broadcast; no promiscuous mode. */
    MAC_REGISTER(0x18) = 0; /* No pause advertised or automatic flow control. */
    /* PS=MII/RMII, DO=disable receive-own; ACS/CST stay clear, so FL includes FCS. */
    uint32_t config = UINT32_C(0x0000a000);
    if (speed == 100)
        config |= UINT32_C(0x00004000);
    if (duplex)
        config |= UINT32_C(0x00000800);
    MAC_REGISTER(0) = config | (link ? MAC_ENABLE : 0);
    MAC_REGISTER(0x1014) = DMA_W1C;
    dma_sync();
    MAC_REGISTER(0x1018) = DMA_STORE_FORWARD | (link ? DMA_START : 0);
    if (link)
        MAC_REGISTER(0x1008) = 1;
}

static int collect_tx(void)
{
    if (!hardware_owned || !driver.initialized)
        return -ENODEV;
    if (dma_fault)
        return driver.last_error;
    uint32_t status = MAC_REGISTER(0x1014);
    driver.dma_status = status;
    if (status & DMA_FATAL)
        return driver_fail(-EIO);
    if (!tx_pending)
        return 0;
    uint32_t descriptor = ((volatile dma_descriptor_t *)&dma->tx)->status;
    if (descriptor & DESC_OWN) {
        if (platform_time_us() >= tx_deadline) {
            ++driver.tx_errors;
            return driver_fail(-ETIMEDOUT);
        }
        return -EAGAIN;
    }
    dma_sync();
    driver.last_tx_descriptor = descriptor;
    tx_pending = false;
    if (descriptor & DESC_ERROR) {
        ++driver.tx_errors;
        driver.last_error = -EIO;
    } else {
        ++driver.tx_packets;
    }
    MAC_REGISTER(0x1014) = status & DMA_W1C;
    return 0;
}

int ai_board_gmac_init(const uint8_t mac[6])
{
    if (!mac || (mac[0] & 1U))
        return -EINVAL;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 6; ++i)
        nonzero |= mac[i];
    if (!nonzero)
        return -EINVAL;
    if (hardware_owned)
        return -EBUSY;
    uint32_t apll = pll_rate(CPM_Instance->CPAPCR);
    uint32_t mpll = pll_rate(CPM_Instance->CPMPCR);
    uint32_t h2 = bus_rate(CPM_Instance->CPCCR, 24, 12, apll, mpll);
    if (!h2 || h2 > 300000000 || !mpll || mpll % 50000000 ||
        mpll / 50000000 > 256 || CPM_Instance->CPMPACR || CPM_Instance->CPAPACR)
        return -ERANGE;
    /* Preserve the board-proven management configuration; do not guess mode4. */
    if ((CPM_Instance->MACPHYC0 & 7U) != 2U)
        return -ENOTSUP;
    memset(&driver, 0, sizeof(driver));
    driver.phy_address = UINT8_MAX;
    driver.phy_mode = CPM_Instance->MACPHYC0;
    saved_clock = CPM_Instance->MACPHYCDR;
    unsigned long flags = _local_irq_save();
    saved_gate = CPM_Instance->CLKGR0 & MAC_GATE;
    CPM_Instance->CLKGR0 &= ~MAC_GATE;
    (void)CPM_Instance->CLKGR0;
    local_irq_restore(flags);
    if ((MAC_REGISTER(0) & MAC_ENABLE) || (MAC_REGISTER(0x1018) & DMA_START)) {
        flags = _local_irq_save();
        CPM_Instance->CLKGR0 |= saved_gate;
        local_irq_restore(flags);
        return -EBUSY;
    }
    for (unsigned i = 0; i < 10; ++i)
        saved_pins[i] = LL_GPIO_getPinMode(PC_Instance, 15 + i);
    hardware_owned = true;
    dma_fault = false;
    tx_pending = false;
    memcpy(station_address, mac, sizeof(station_address));
    int result = clock_write(saved_clock | CLOCK_STOP);
    if (!result)
        result = clock_write((UINT32_C(1) << 30) | (mpll / 50000000 - 1));
    if (result)
        goto failed;
    LL_GPIO_setPinMaskMode(PC_Instance, UINT32_C(0x3ff) << 15, GPIO_MODE_INPUT);
    LL_GPIO_setPinMode(PC_Instance, 15, GPIO_MODE_OUTPUT0);
    delay_us(10000);
    LL_GPIO_setPinMode(PC_Instance, 15, GPIO_MODE_OUTPUT1);
    delay_us(10000);
    LL_GPIO_setPinMaskMode(PC_Instance, UINT32_C(0x3ff) << 15, GPIO_MODE_FUNCTION0);
    delay_us(10);
    result = controller_reset();
    if (result)
        goto failed;
    driver.mac_version = MAC_REGISTER(0x20);
    driver.hardware_features = MAC_REGISTER(0x1058);
    if (!(driver.hardware_features & (UINT32_C(1) << 24))) {
        result = -ENOTSUP;
        goto failed;
    }
    for (unsigned address = 0; address < 32; ++address) {
        uint16_t id1, id2;
        result = mdio_read(address, 2, &id1);
        if (!result)
            result = mdio_read(address, 3, &id2);
        if (result)
            goto failed;
        if (id1 == 0x0243 && (id2 & 0xfff0U) == 0x0c50) {
            driver.phy_address = (uint8_t)address;
            driver.phy_id = ((uint32_t)id1 << 16) | id2;
            break;
        }
    }
    if (driver.phy_address == UINT8_MAX) {
        result = -ENODEV;
        goto failed;
    }
    /* IEEE 802.3 ANAR: selector1, 10/100 half/full, no pause or next page.
     * BMCR: autonegotiation enable/restart. Completion is polled later. */
    result = mdio_write(driver.phy_address, 4, 0x01e1);
    if (!result)
        result = mdio_write(driver.phy_address, 0, 0x1200);
    if (result)
        goto failed;
    CleanInvalidateDCache_by_Addr((unsigned long *)&dma_memory, sizeof(dma_memory));
    dma = (dma_memory_t *)(uintptr_t)(physical(&dma_memory) | UINT32_C(0xa0000000));
    configure_dma(false, 100, true);
    driver.initialized = true;
    return 0;
failed:
    driver.last_error = result;
    (void)ai_board_gmac_stop();
    return result;
}

int ai_board_gmac_poll_link(void)
{
    int result = collect_tx();
    if (result && result != -EAGAIN)
        return result;
    uint16_t status, control, advertisement, partner;
    result = mdio_read(driver.phy_address, 1, &status);
    if (!result)
        result = mdio_read(driver.phy_address, 1, &status); /* latch-low */
    if (!result)
        result = mdio_read(driver.phy_address, 0, &control);
    if (!result)
        result = mdio_read(driver.phy_address, 4, &advertisement);
    if (!result)
        result = mdio_read(driver.phy_address, 5, &partner);
    if (result)
        return driver_fail(result);
    driver.bmsr = status;
    driver.bmcr = control;
    driver.advertisement = advertisement;
    driver.partner = partner;
    bool link = (status & 4U) != 0;
    bool full = false;
    unsigned speed = 0;
    if (link && (control & 0x1000U)) {
        link = (status & 0x0020U) != 0; /* Do not wait for negotiation here. */
        uint16_t common = advertisement & partner;
        if (common & 0x0100U) { speed = 100; full = true; }
        else if (common & 0x0080U) { speed = 100; }
        else if (common & 0x0040U) { speed = 10; full = true; }
        else if (common & 0x0020U) { speed = 10; }
        else link = false;
    } else if (link) {
        speed = (control & 0x2000U) ? 100 : 10;
        full = (control & 0x0100U) != 0;
    }
    if (!link) { speed = 0; full = false; }
    if (link != driver.link_up || speed != driver.speed_mbps || full != driver.full_duplex) {
        result = controller_reset();
        if (result)
            return driver_fail(result);
        configure_dma(link, speed, full);
        driver.link_up = link;
        driver.speed_mbps = speed;
        driver.full_duplex = full;
    }
    return 0;
}

int ai_board_gmac_send(const void *frame, size_t length)
{
    if (!frame || length < 14)
        return -EINVAL;
    if (length > 1518)
        return -EMSGSIZE;
    int result = collect_tx();
    if (result)
        return result;
    if (!driver.link_up)
        return -ENETDOWN;
    memcpy(dma->tx_buffer, frame, length);
    dma->tx.length = (uint32_t)length;
    dma->tx.buffer1 = physical(dma->tx_buffer);
    dma->tx.buffer2 = 0;
    dma->tx.extended = 0;
    dma_sync();
    tx_pending = true;
    tx_deadline = platform_time_us() + 100000;
    ((volatile dma_descriptor_t *)&dma->tx)->status = TX_FIRST_LAST_END | DESC_OWN;
    dma_sync();
    MAC_REGISTER(0x1014) = UINT32_C(4); /* TX buffer unavailable, W1C. */
    MAC_REGISTER(0x1004) = 1;
    return 0;
}

int ai_board_gmac_read(void *frame, size_t capacity)
{
    if (!frame || capacity < 14)
        return -EINVAL;
    int result = collect_tx();
    if (result && result != -EAGAIN)
        return result;
    if (!driver.link_up)
        return 0;
    volatile dma_descriptor_t *descriptor = &dma->rx[next_rx];
    uint32_t status = descriptor->status;
    if (status & DESC_OWN)
        return 0;
    dma_sync();
    driver.last_rx_descriptor = status;
    unsigned length = (status >> 16) & 0x3fff;
    if ((status & DESC_ERROR) || (status & RX_FIRST_LAST) != RX_FIRST_LAST ||
        length < 18 || length > DMA_BUFFER_SIZE) {
        ++driver.rx_errors;
        ++driver.rx_dropped;
        result = -EIO;
    } else if (length - 4 > capacity) {
        ++driver.rx_dropped;
        result = -EMSGSIZE;
    } else {
        memcpy(frame, dma->rx_buffer[next_rx], length - 4);
        ++driver.rx_packets;
        result = (int)length - 4;
    }
    dma_sync();
    descriptor->status = DESC_OWN;
    dma_sync();
    next_rx = (next_rx + 1) % RX_COUNT;
    MAC_REGISTER(0x1014) = UINT32_C(0xc0); /* RU/RI W1C, then resume RX. */
    MAC_REGISTER(0x1008) = 1;
    return result;
}

void ai_board_gmac_status(ai_gmac_status_t *report)
{
    if (driver.initialized) {
        (void)collect_tx();
        sample_mmc();
    }
    if (report)
        *report = driver;
}

int ai_board_gmac_stop(void)
{
    if (!hardware_owned)
        return 0;
    int result = controller_reset();
    if (result)
        return driver_fail(result);
    driver.initialized = false;
    driver.link_up = false;
    driver.speed_mbps = 0;
    driver.full_duplex = false;
    result = clock_write(saved_clock);
    if (result)
        return driver_fail(result);
    for (unsigned i = 0; i < 10; ++i)
        LL_GPIO_setPinMode(PC_Instance, 15 + i, saved_pins[i]);
    unsigned long flags = _local_irq_save();
    if (saved_gate)
        CPM_Instance->CLKGR0 |= MAC_GATE;
    local_irq_restore(flags);
    hardware_owned = false;
    return 0;
}
