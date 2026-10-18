"""Derive the HALLEY7-M display drivers without editing the vendor SDK.

The PHY timings below were read from the working ZC50289HSHD02 Linux setup
on 2026-10-16. They apply to its 720x1280, two-lane, nominal 30 Hz mode.
"""

import argparse
import hashlib
import json
from pathlib import Path


SOURCE_NAMES = (
    "x2600_hal_lcd.c",
    "x2600_hal_mipi_dsi.c",
    "x2600_ll_mipi_dsi.c",
)


def replace_expected(text, old, new, count=1):
    observed = text.count(old)
    if observed != count:
        raise RuntimeError(
            f"Display SDK patch expected {count} matches, found {observed}: "
            f"{old[:100]!r}"
        )
    return text.replace(old, new, count)


def derive_lcd(text):
    text = replace_expected(
        text,
        '#include "x2600_hal.h"',
        '''#include "x2600_hal.h"

/* Keep DMA descriptors on complete cache lines for MIPS D/L2 publication. */
_Static_assert(_Alignof(LCD_HandleTypeDef) >= 64, "LCD handle alignment");
_Static_assert(__builtin_offsetof(LCD_HandleTypeDef, RdmaData.RdmaDesc) % 64 == 0,
               "RDMA descriptor array alignment");
_Static_assert(sizeof(struct Rdma_Desc) == 64, "RDMA descriptor cache line");''',
    )
    return replace_expected(
        text,
        "    Flush_Cache_AllAddr();",
        '''    /* Framebuffer contents are published by the display backend.
     * Publish the complete aligned descriptor array through both cache levels. */
    CleanDCache_by_Addr((unsigned long *)rdma_data->RdmaDesc,
                       sizeof(rdma_data->RdmaDesc));''',
    )


def derive_mipi_hal(text):
    text = replace_expected(
        text,
        "static struct jz_dsi *jz_dsi;",
        "static struct jz_dsi regba_dsi;\n"
        "static struct jz_dsi *const jz_dsi = &regba_dsi;",
    )
    # Both packet functions test negative errors returned by their LL helpers.
    text = replace_expected(text, "    unsigned int ret;", "    int ret;", 2)
    text = replace_expected(
        text,
        "void jz_dsi_video_cfg(void)\n{\n    dsi_set_power(jz_dsi, 1);",
        "void jz_dsi_video_cfg(void)\n{\n"
        "    /* DPI/VID static registers must be configured while idle. */\n"
        "    dsi_set_power(jz_dsi, 0);",
    )
    text = replace_expected(
        text,
        '    prom_printk("dsi phy address = 0x%p\\n", jz_dsi->DPHY_TX_Instance);\n\n',
        "",
    )
    return replace_expected(
        text,
        "    jz_dsi->DPHY_TX_Instance = MIPI_DPHY_TX_Instance;",
        '''    jz_dsi->DPHY_TX_Instance = MIPI_DPHY_TX_Instance;
    prom_printk("dsi phy address = 0x%p\\n", jz_dsi->DPHY_TX_Instance);''',
    )


PHY_HELPERS = '''/* HALLEY7-M / ZC50289HSHD02 two-lane 30 Hz PHY configuration.
 * Captured with read-only devmem from the working Linux driver:
 * build/linux-display-registers-20261016.log. PN swapping remains controlled
 * by the panel's video_config rather than this timing table. */
static void regba_dphy_lane_timings(struct jz_dsi *dsi)
{
    static const unsigned int lane_offsets[] = {
        0x500, 0x580, 0x600, 0x680, 0x700
    };
    volatile unsigned int *phy =
        (volatile unsigned int *)dsi->DPHY_TX_Instance;
    unsigned int lane;
    for (lane = 0; lane < 5; ++lane) {
        unsigned int base = lane_offsets[lane] / sizeof(unsigned int);
        WRITE_REG(phy[base + 0x14 / sizeof(unsigned int)], 0x05);
        WRITE_REG(phy[base + 0x18 / sizeof(unsigned int)], 0x40);
        WRITE_REG(phy[base + 0x1c / sizeof(unsigned int)],
                  lane == 0 ? 0x1e : 0x08);
        WRITE_REG(phy[base + 0x20 / sizeof(unsigned int)], 0x20);
    }
}

static void regba_dphy_analog_tuning(struct jz_dsi *dsi)
{
    DPHY_TX_TypeDef *phy = dsi->DPHY_TX_Instance;
    WRITE_REG(phy->ANA_REG05, (READ_REG(phy->ANA_REG05) & ~0xffU) | 0x08);
    WRITE_REG(phy->ANA_REG06, (READ_REG(phy->ANA_REG06) & ~0xffU) | 0x00);
    WRITE_REG(phy->ANA_REG07, (READ_REG(phy->ANA_REG07) & ~0xffU) | 0x88);
    WRITE_REG(phy->ANA_REG08, (READ_REG(phy->ANA_REG08) & ~0xffU) | 0x67);
    WRITE_REG(phy->ANA_REG0B, (READ_REG(phy->ANA_REG0B) & ~0xffU) | 0x5f);
}

'''


def derive_mipi_ll(text):
    text = replace_expected(
        text,
        "static int jz_dsih_dphy_configure_x2600(struct jz_dsi *dsi)",
        PHY_HELPERS + "static int jz_dsih_dphy_configure_x2600(struct jz_dsi *dsi)",
    )
    text = replace_expected(
        text,
        "    if (dsi->video_config.clk_lane_pn_swap) {",
        "    regba_dphy_lane_timings(dsi);\n\n"
        "    if (dsi->video_config.clk_lane_pn_swap) {",
    )
    return replace_expected(
        text,
        '''        prom_printk("DSI_REG 700 : 0x%x\\n", dsi->DPHY_TX_Instance->DATA_LANE3_REG00);
    }
    return 0;
}''',
        '''        prom_printk("DSI_REG 700 : 0x%x\\n", dsi->DPHY_TX_Instance->DATA_LANE3_REG00);
    }
    regba_dphy_analog_tuning(dsi);
    return 0;
}''',
    )


DERIVATIONS = {
    "x2600_hal_lcd.c": (
        derive_lcd,
        ["assert MIPS DMA descriptor alignment", "publish RDMA array through D/L2"],
    ),
    "x2600_hal_mipi_dsi.c": (
        derive_mipi_hal,
        ["allocate static DSI handle", "retain signed packet errors", "log initialized PHY pointer",
         "configure video static registers while idle, matching board U-Boot"],
    ),
    "x2600_ll_mipi_dsi.c": (
        derive_mipi_ll,
        ["apply measured ZC502 lane timings", "apply measured PHY analog tuning"],
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="libbare-cpu SDK root")
    parser.add_argument("--output", type=Path, required=True, help="derived build directory")
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    source_dir = sdk / "drivers" / "drivers-x2600" / "src"
    output = args.output.resolve()
    if output == sdk or sdk in output.parents:
        parser.error("--output must stay outside the vendor SDK")

    # Validate every source patch before writing any generated file.
    generated = {}
    manifest = {
        "schema": 1,
        "sdk": str(sdk),
        "phy_reference": "build/linux-display-registers-20261016.log",
        "panel_mode": "HALLEY7-M ZC50289HSHD02 720x1280 two-lane nominal 30 Hz",
        "sources": {},
    }
    for name in SOURCE_NAMES:
        raw = (source_dir / name).read_bytes()
        original = raw.decode("utf-8").replace("\r\n", "\n")
        derive, changes = DERIVATIONS[name]
        result = derive(original).encode("utf-8")
        generated[name] = result
        manifest["sources"][name] = {
            "input_sha256": hashlib.sha256(raw).hexdigest(),
            "output_sha256": hashlib.sha256(result).hexdigest(),
            "changes": changes,
        }
    output.mkdir(parents=True, exist_ok=True)
    for name, content in generated.items():
        (output / name).write_bytes(content)
    (output / "SOURCE.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"Prepared {len(generated)} display drivers in {output}")


if __name__ == "__main__":
    main()
