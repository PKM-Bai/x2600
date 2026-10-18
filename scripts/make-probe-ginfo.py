"""Build Cloner 2.5.54 RAM-loader parameters; this script never accesses USB."""

import hashlib
import json
import re
import struct
from pathlib import Path


REGISTER_FIELDS = (
    "DDRC_CFG_VALUE", "DDRC_CTRL_VALUE", "DDRC_DLMR_VALUE", "DDRC_DDLP_VALUE",
    "DDRC_MMAP0_VALUE", "DDRC_MMAP1_VALUE", "DDRC_REFCNT_VALUE",
    "DDRC_TIMING1_VALUE", "DDRC_TIMING2_VALUE", "DDRC_TIMING3_VALUE",
    "DDRC_TIMING4_VALUE", "DDRC_TIMING5_VALUE", "DDRC_AUTOSR_CNT_VALUE",
    "DDRC_AUTOSR_EN_VALUE", "DDRC_HREGPRO_VALUE", "DDRC_PREGPRO_VALUE",
    "DDRC_CGUC0_VALUE", "DDRC_CGUC1_VALUE", "DDRP_MEMCFG_VALUE",
    "DDRP_CL_VALUE", "DDRP_CWL_VALUE", "DDR_MR0_VALUE", "DDR_MR1_VALUE",
    "DDR_MR2_VALUE", "DDR_MR3_VALUE", "DDR_MR10_VALUE", "DDR_MR11_VALUE",
    "DDR_MR63_VALUE", "DDR_CHIP_0_SIZE", "DDR_CHIP_1_SIZE",
)


def field(body: str, name: str) -> int:
    match = re.search(r"\." + re.escape(name) + r"\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*,", body)
    if not match:
        raise ValueError(f"Missing generated DDR field: {name}")
    value = int(match.group(1), 0)
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError(f"DDR field exceeds uint32: {name}")
    return value


def tlv(magic: int, payload: bytes) -> bytes:
    return struct.pack("<II", magic, len(payload)) + payload


def main() -> None:
    workspace = Path(__file__).resolve().parent.parent
    output_dir = workspace / "build" / "flash-probe"
    header_path = output_dir / "ddr_reg_values_400mhz.h"
    text = header_path.read_text(encoding="utf-8")
    if re.search(r"^\s*#error", text, re.MULTILINE):
        raise ValueError("The official DDR generator reported an error")
    profiles = list(re.finditer(r'\.h\.name\s*=\s*"([^\"]+)"', text))
    selected = [index for index, match in enumerate(profiles) if match.group(1) == "X2600M-DDR2"]
    if len(selected) != 1:
        raise ValueError("Expected exactly one X2600M-DDR2 profile")
    index = selected[0]
    end = profiles[index + 1].start() if index + 1 < len(profiles) else len(text)
    body = text[profiles[index].start():end]

    profile_id = field(body, "h.id")
    generated_type = field(body, "h.type")
    frequency = field(body, "h.freq")
    registers = {name: field(body, name) for name in REGISTER_FIELDS}
    remap = [field(body, f"REMMAP_ARRAY[{index}]") for index in range(6)]
    if profile_id != 0xC2 or generated_type != 5 or frequency != 400_000_000:
        raise ValueError("Expected the generated 400 MHz X2600M DDR2 profile with ID 0xC2 and source type 5")
    if registers["DDR_CHIP_0_SIZE"] != 64 * 1024 * 1024 or registers["DDR_CHIP_1_SIZE"] != 0:
        raise ValueError("Expected one 64 MiB DDR bank")
    if registers["DDRP_CL_VALUE"] != 7:
        raise ValueError("Expected the official 400 MHz DDR2 profile's CL7")

    # The current U-Boot generator uses type 5; the deployed Cloner 2.5.54 ABI uses type 4.
    ddr = struct.pack("<32sIII", b"X2600M-DDR2", profile_id, 4, frequency)
    ddr += struct.pack("<36I", *registers.values(), *remap)
    if len(ddr) != 188:
        raise ValueError("Cloner X2600 DDR payload must be 188 bytes")

    # UART0 PE09, then one SFC entry: PD6..PD11, FUNC0_PULL.
    gpio = struct.pack("<4I", 4, 1, 0x200, 1)
    gpio += struct.pack("<3I", 3, 0x10, 0xFC0) + bytes(11 * 12)
    board = struct.pack("<6I", 24_000_000, 800_000_000, frequency, 0, 0, 115_200) + gpio
    if len(board) != 184:
        raise ValueError("Cloner board and GPIO payload must be 184 bytes")
    ginfo = tlv(0x424F4F54, struct.pack("<I", 0))
    ginfo += tlv(0x42444946, board)
    ginfo += tlv(0x44445200, ddr)
    ginfo += struct.pack("<I", 0)
    if len(ginfo) != 404:
        raise ValueError("Expected 400 bytes of parameters plus a zero magic terminator")

    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "ginfo.bin"
    output_path.write_bytes(ginfo)
    print(json.dumps({
        "header": str(header_path), "output": str(output_path),
        "bytes": len(ginfo), "sha256": hashlib.sha256(ginfo).hexdigest(),
        "profile": "X2600M-DDR2", "profile_id": "0xC2",
        "generated_type": generated_type, "cloner_abi_type": 4,
        "extal_hz": 24_000_000, "cpu_hz": 800_000_000,
        "ddr_hz": frequency, "ddr_bytes": registers["DDR_CHIP_0_SIZE"],
        "ddr_payload_bytes": len(ddr), "board_payload_bytes": len(board),
        "uart": "UART0 PE09, 115200", "sfc": "PD6..PD11, FUNC0_PULL",
        "registers": {name: f"0x{value:08X}" for name, value in registers.items()},
        "remap": [f"0x{value:08X}" for value in remap],
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
