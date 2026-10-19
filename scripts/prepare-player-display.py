#!/usr/bin/env python3
"""Derive the player's NV12 composer drivers without modifying the SDK."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

BASE_SCRIPT = Path(__file__).with_name("prepare-display.py")
SPEC = importlib.util.spec_from_file_location("player_board_display", BASE_SCRIPT)
BASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASE)


def derive_composer(text):
    text = BASE.derive_lcd(text)
    text = BASE.replace_expected(text,
        "comp_data->LayerDesc[next_frame][j].LayerCfg.b.domain_multi = 1;",
        "comp_data->LayerDesc[next_frame][j].LayerCfg.b.domain_multi = lay_cfg[j].domain_multi;")
    text = BASE.replace_expected(text, "comp_data->csc_mode = CSC_MODE_1;",
                                 "comp_data->csc_mode = CSC_MODE_2;")
    text = BASE.replace_expected(text,
        "    WRITE_REG(hlcd->Instance->DC_FRM_CFG_ADDR, CPHYSADDR(&comp_data->FrameDesc[comp_data->comp_next_frame]));",
        """    /* Publish only the newly built descriptors through D/L2, before MMIO.
     * Pixel buffers are published by their producer, including FELIX DMA. */
    CleanDCache_by_Addr((unsigned long *)&comp_data->FrameDesc[comp_data->comp_next_frame],
                       sizeof(comp_data->FrameDesc[0]));
    CleanDCache_by_Addr((unsigned long *)comp_data->LayerDesc[comp_data->comp_next_frame],
                       sizeof(comp_data->LayerDesc[0]));
    WRITE_REG(hlcd->Instance->DC_FRM_CFG_ADDR, CPHYSADDR(&comp_data->FrameDesc[comp_data->comp_next_frame]));""")
    return BASE.replace_expected(text,
        '_Static_assert(_Alignof(LCD_HandleTypeDef) >= 64, "LCD handle alignment");',
        '''_Static_assert(_Alignof(LCD_HandleTypeDef) >= 64, "LCD handle alignment");
_Static_assert(sizeof(struct FrameDesc) == 64, "composer frame cache line");
_Static_assert(sizeof(struct LayerDesc) == 64, "composer layer cache line");
_Static_assert(__builtin_offsetof(LCD_HandleTypeDef, CompData.FrameDesc) % 64 == 0,
               "composer frame array alignment");
_Static_assert(__builtin_offsetof(LCD_HandleTypeDef, CompData.LayerDesc) % 64 == 0,
               "composer layer array alignment");''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sdk, output = args.sdk.resolve(), args.output.resolve()
    if output == sdk or sdk in output.parents:
        parser.error("--output must stay outside the vendor SDK")
    generated = {}
    manifest = dict(schema=1, sdk=str(sdk), pixel_format="NV12", csc_mode="CSC_MODE_2",
                    domain_multi="per layer, straight alpha=0", sources={})
    for name in BASE.SOURCE_NAMES:
        raw = (sdk / "drivers/drivers-x2600/src" / name).read_bytes()
        original = raw.decode("utf-8").replace("\r\n", "\n")
        derive = derive_composer if name == "x2600_hal_lcd.c" else BASE.DERIVATIONS[name][0]
        result = derive(original).encode("utf-8")
        generated[name] = result
        manifest["sources"][name] = dict(input_sha256=hashlib.sha256(raw).hexdigest(),
                                         output_sha256=hashlib.sha256(result).hexdigest())
    output.mkdir(parents=True, exist_ok=True)
    for name, content in generated.items():
        (output / name).write_bytes(content)
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(generated)} player composer/DSI drivers in {output}")


if __name__ == "__main__":
    main()
