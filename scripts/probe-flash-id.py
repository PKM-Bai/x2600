"""Load the official USB RAM helper and request JEDEC ID without VR_INIT.

This probe sends no VR_INIT, erase, program, EFUSE or security request.
The RAM helper initializes the SFC controller and issues a volatile NOR reset,
then reads JEDEC ID. It does not write Flash data or status registers.
Reset into USB Boot again after the probe before using Cloner.
"""
from array import array
import configparser
import json
from pathlib import Path
import struct
import time

import usb.backend.libusb0
import usb.core
import usb.util

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "tools/cloner-windows/cloner-2.5.54-windows_alpha"
CLONER = PACKAGE / "cloner-2.5.54-windows_alpha"
OUTPUT = ROOT / "build/flash-probe"
DLL = PACKAGE / "cloner-win32-driver/amd64/libusb0.dll"


def main():
    ginfo = (OUTPUT / "ginfo.bin").read_bytes()
    # Reserve room for the stock SPL to zero its appended PHY parameters.
    if len(ginfo) > 1024:
        raise SystemExit("RAM parameter block is too large for the reserved area.")
    ginfo = ginfo.ljust(1024, b"\0")
    spl = (CLONER / "firmwares/x2600/spl.bin").read_bytes()
    helper = (CLONER / "firmwares/x2600/uboot.bin").read_bytes()
    if len(spl) != 12348 or len(helper) != 312456:
        raise SystemExit("This probe expects the deployed Cloner 2.5.54 X2600 RAM firmware.")

    backend = usb.backend.libusb0.get_backend(find_library=lambda name: str(DLL))
    if backend is None:
        raise SystemExit("Cloner libusb0 backend is unavailable.")
    device = usb.core.find(idVendor=0xA108, idProduct=0xEAEF, backend=backend)
    if device is None:
        raise SystemExit("X2600 USB device not found. Enter USB Boot first.")
    usb.util.claim_interface(device, 0)
    # libusb0 bulk API avoids BootROM's unsupported GET_CONFIGURATION request.
    handle = device._ctx.handle

    def control_read(request, length):
        return bytes(device.ctrl_transfer(0xC0, request, 0, 0, length, timeout=2000))

    def control_out(request, number=0, payload=b""):
        # The probe explicitly excludes VR_INIT(0x11) and VR_WRITE(0x12).
        if request not in (0x01, 0x02, 0x03, 0x04, 0x05, 0x14):
            raise ValueError("Request is outside the RAM probe protocol.")
        result = device.ctrl_transfer(
            0x40, request, number >> 16, number & 0xFFFF, payload, timeout=2000
        )
        if result != len(payload):
            raise RuntimeError(f"Control request {request:#x} returned {result}.")

    def bulk_out(data):
        result = backend.bulk_write(handle, 0x01, 0, array("B", data), 10000)
        if result != len(data):
            raise RuntimeError(f"RAM transfer was short: {result}/{len(data)}.")

    def upload_ram(address, data):
        control_out(0x01, address)
        control_out(0x02, len(data))
        bulk_out(data)

    try:
        cpu = control_read(0x00, 5)
        if cpu != b"X2600":
            raise RuntimeError(f"Unexpected CPU response: {cpu!r}. Reset into USB Boot.")
        print("CPU:", cpu.decode(), flush=True)
        print("Loading RAM parameters: CPU 800MHz, DDR2 400MHz, 64MiB.", flush=True)
        upload_ram(0x80001000, ginfo)
        upload_ram(0x80001800, spl)
        control_out(0x02, 0x7000)
        print("Running official RAM SPL.", flush=True)
        control_out(0x04, 0x80001800)
        time.sleep(1.1)
        print("RAM SPL CPU response:", control_read(0x00, 5), flush=True)

        upload_ram(0x80100000, helper)
        control_out(0x03)
        print("Running official RAM USB helper.", flush=True)
        control_out(0x05, 0x80100000)
        time.sleep(1.1)
        print("RAM helper CPU response:", control_read(0x00, 5), flush=True)

        policy = bytearray(164)
        struct.pack_into("<I", policy, 20, 1)  # use_sfc_nor; other flags stay zero.
        parameters = struct.pack("<II", 0x504F4C49, len(policy)) + policy + bytes(8)
        command = struct.pack("<I", len(parameters)) + bytes(28)
        control_out(0x14, payload=command)
        time.sleep(0.1)
        bulk_out(parameters)
        time.sleep(0.1)
        ack = struct.unpack("<i", control_read(0x10, 4))[0]
        if ack:
            raise RuntimeError(f"RAM policy acknowledgement failed: {ack}.")

        raw = control_read(0x26, 3)
        chip_id = int.from_bytes(raw, "little")
        print(f"Flash info raw: {raw.hex()}, ID: 0x{chip_id:06x}", flush=True)
        db = configparser.ConfigParser(interpolation=None)
        db.read(CLONER / "configs/spiflashinfo.cfg")
        matches = []
        for section in db.sections():
            if not section.startswith("norkey"):
                continue
            fields = db[section].get("value", "").strip('"').split(",")
            if len(fields) >= 3 and int(fields[1], 16) == chip_id:
                matches.append({"model": fields[0], "capacity_bytes": int(fields[2])})
        result = {"raw": raw.hex(), "id": f"0x{chip_id:06x}", "nor_matches": matches}
        (OUTPUT / "flash-id.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        for match in matches:
            print("NOR model:", match["model"], "capacity:", match["capacity_bytes"], "bytes", flush=True)
        if not matches:
            print("No confirmed NOR match. Retain the raw ID; do not select a burn profile from this alone.", flush=True)
        print("Probe finished. No VR_INIT or Flash programming request was sent.", flush=True)
    finally:
        usb.util.dispose_resources(device)


if __name__ == "__main__":
    main()
