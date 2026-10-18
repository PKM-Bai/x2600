"""Back up this board's 16MiB NOR using the stock Cloner RAM helper.

Default: inspect local inputs and print the plan, without opening USB.
--execute: BootROM RAM upload, RAM policy, ID, VR_READ/CRC/ACK only.
VR_INIT and VR_WRITE are absent from the request allowlist. No Flash erase,
program, status-register write, security operation, or reboot is sent.
Reset into USB Boot again when the actual backup session is finished.
"""
from __future__ import annotations

import argparse
from array import array
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "tools/cloner-windows/cloner-2.5.54-windows_alpha"
CLONER = PACKAGE / "cloner-2.5.54-windows_alpha"
FLASH_BYTES = 16 * 1024 * 1024
CHUNK_BYTES = 64 * 1024
EXPECTED_ID = 0x852018
SFC_NOR_OPS = 6 << 16
# All OUT transfers target BootROM RAM or the helper's read/config dispatcher.
ALLOWED_OUT = frozenset((0x01, 0x02, 0x03, 0x04, 0x05, 0x13, 0x14))
ALLOWED_IN = frozenset((0x00, 0x10, 0x19, 0x26))
ASSETS = {
    "ginfo": (ROOT / "build/flash-probe/ginfo.bin", 404,
              "81ed993867ee297aeb56430561b64694ea6b5d620b9c70892ef12adea502a99a"),
    "spl": (CLONER / "firmwares/x2600/spl.bin", 12348,
            "3f04a5fe3e9a0c68ef49b0bd542d501cc113ab6ef9032e01c56d80d2f0710692"),
    "helper": (CLONER / "firmwares/x2600/uboot.bin", 312456,
               "a11c4fa5ce8c8eb023828682c07c182fec6ef6ab1b399f0e46602f5419ffba28"),
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def crc_raw(data: bytes) -> int:
    # SDK local_crc32(seed=0xffffffff) does not perform a final complement.
    return zlib.crc32(data) ^ 0xffffffff


def input_files() -> tuple[dict[str, bytes], dict]:
    data, description = {}, {}
    for name, (path, size, sha) in ASSETS.items():
        payload = path.read_bytes()
        if len(payload) != size or digest(payload) != sha:
            raise RuntimeError(f"Unrecognized {name} input: {path}; expected size and SHA256 must match.")
        data[name] = payload
        description[name] = {"path": str(path), "bytes": size, "sha256": sha}
    return data, description


def ram_policy() -> bytes:
    policy = bytearray(164)
    struct.pack_into("<I", policy, 20, 1)  # use_sfc_nor only
    debug = struct.pack("<5I", 0, 1, 0, CHUNK_BYTES, 30)
    return (struct.pack("<II", 0x504f4c49, len(policy)) + policy +
            struct.pack("<II", 0x44424700, len(debug)) + debug + bytes(8))


class UsbTransport:
    def __init__(self):
        # Imported only for explicit --execute; default/self-test never opens USB.
        import usb.backend.libusb0
        import usb.core
        import usb.util
        self.util = usb.util
        dll = PACKAGE / "cloner-win32-driver/amd64/libusb0.dll"
        self.backend = usb.backend.libusb0.get_backend(find_library=lambda name: str(dll))
        if self.backend is None:
            raise RuntimeError("Stock Cloner libusb0 backend unavailable.")
        self.device = usb.core.find(idVendor=0xa108, idProduct=0xeaef, backend=self.backend)
        if self.device is None:
            raise RuntimeError("X2600 not found; manually enter USB Boot before --execute.")
        usb.util.claim_interface(self.device, 0)
        self.handle = self.device._ctx.handle
        self.out_requests = []

    def out(self, request: int, number: int = 0, payload: bytes = b"") -> None:
        if request not in ALLOWED_OUT:
            raise ValueError(f"OUT request {request:#x} is not in the read-only backup protocol.")
        self.out_requests.append(request)
        result = self.device.ctrl_transfer(0x40, request, number >> 16,
                                          number & 0xffff, payload, timeout=10000)
        if result != len(payload):
            raise RuntimeError(f"Short control OUT {request:#x}: {result}/{len(payload)}")

    def control(self, request: int, size: int) -> bytes:
        if request not in ALLOWED_IN:
            raise ValueError("Control IN request is not in the backup protocol.")
        data = bytes(self.device.ctrl_transfer(0xc0, request, 0, 0, size, timeout=10000))
        if len(data) != size:
            raise RuntimeError(f"Short control IN {request:#x}: {len(data)}/{size}")
        return data

    def send(self, data: bytes) -> None:
        sent = self.backend.bulk_write(self.handle, 0x01, 0, array("B", data), 10000)
        if sent != len(data):
            raise RuntimeError(f"Short RAM/config bulk OUT: {sent}/{len(data)}")

    def receive(self, size: int) -> bytes:
        buffer = array("B", bytes(size))
        actual = self.backend.bulk_read(self.handle, 0x81, 0, buffer, 10000)
        if actual != size:
            raise RuntimeError(f"Short Flash bulk IN: {actual}/{size}")
        return buffer.tobytes()

    def close(self):
        self.util.dispose_resources(self.device)


def ack(transport) -> None:
    value, = struct.unpack("<i", transport.control(0x10, 4))
    if value != 0:
        raise RuntimeError(f"RAM helper acknowledgement failed: {value}")


def load_helper(transport, assets: dict[str, bytes]) -> int:
    if transport.control(0x00, 5) != b"X2600":
        raise RuntimeError("Unexpected CPU; reset into X2600 USB Boot.")

    def upload(address: int, data: bytes):
        transport.out(0x01, address)
        transport.out(0x02, len(data))
        transport.send(data)

    # Same official RAM-loading sequence as the already-used Flash ID probe.
    upload(0x80001000, assets["ginfo"].ljust(1024, b"\0"))
    upload(0x80001800, assets["spl"])
    transport.out(0x02, 0x7000)
    transport.out(0x04, 0x80001800)
    time.sleep(1.1)
    if transport.control(0x00, 5) != b"X2600":
        raise RuntimeError("RAM SPL CPU response mismatch.")
    upload(0x80100000, assets["helper"])
    transport.out(0x03)
    transport.out(0x05, 0x80100000)
    time.sleep(1.1)
    if transport.control(0x00, 5) != b"X2600":
        raise RuntimeError("RAM helper CPU response mismatch.")
    policy = ram_policy()
    transport.out(0x14, payload=struct.pack("<I", len(policy)) + bytes(36))
    time.sleep(0.1)
    transport.send(policy)
    time.sleep(0.1)
    ack(transport)
    chip_id = int.from_bytes(transport.control(0x26, 3), "little")
    if chip_id != EXPECTED_ID:
        raise RuntimeError(f"Expected this board's JEDEC 0x{EXPECTED_ID:06x}, got 0x{chip_id:06x}.")
    return chip_id


def read_chunk(transport, offset: int, size: int) -> bytes:
    if offset < 0 or size <= 0 or size > CHUNK_BYTES or offset + size > FLASH_BYTES:
        raise ValueError("Read is outside the fixed 16MiB backup range.")
    # union cmd is 40 bytes: its 9-int RTC member is 36 bytes, rounded up to
    # 8-byte union alignment. Read fields occupy the first 28 bytes.
    packet = struct.pack("<QQQI12x", 0, offset, size, SFC_NOR_OPS)
    transport.out(0x13, payload=packet)
    result, expected_crc = struct.unpack("<iI", transport.control(0x19, 8))
    if result != 0:
        raise RuntimeError(f"NOR read failed at {offset:#x}: {result}")
    # GET_CRC queues the IN transfer. Do not send another control request before
    # consuming it: the stock helper dequeues pending requests in setup().
    data = transport.receive(size)
    if len(data) != size or crc_raw(data) != expected_crc:
        raise RuntimeError(f"NOR length/USB CRC mismatch at {offset:#x}")
    ack(transport)
    return data


def read_pass(transport, path: Path, pass_number: int, progress=True) -> dict:
    sha = hashlib.sha256()
    with path.open("xb") as output:
        for offset in range(0, FLASH_BYTES, CHUNK_BYTES):
            data = read_chunk(transport, offset, min(CHUNK_BYTES, FLASH_BYTES - offset))
            output.write(data)
            sha.update(data)
            if progress and (offset + len(data)) % (1024 * 1024) == 0:
                print(f"Pass {pass_number}: {(offset + len(data)) // (1024 * 1024)}/16 MiB", flush=True)
        output.flush()
        import os
        os.fsync(output.fileno())
    result = {"file": path.name, "bytes": path.stat().st_size, "sha256": sha.hexdigest()}
    if result["bytes"] != FLASH_BYTES or digest(path.read_bytes()) != result["sha256"]:
        raise RuntimeError(f"Persisted backup verification failed: {path}")
    return result


def verify_passes(first: dict, second: dict) -> None:
    if (first["bytes"] != FLASH_BYTES or second["bytes"] != FLASH_BYTES or
            first["sha256"] != second["sha256"]):
        raise RuntimeError("Two full NOR reads differ; preserve results but do not treat them as a verified backup.")


def self_test() -> None:
    class FakeTransport:
        def __init__(self):
            self.offset = self.size = 0
            self.requests = []
            self.data = bytes(range(256)) * (FLASH_BYTES // 256)
            self.bad_crc = False

        def out(self, request, number=0, payload=b""):
            assert request == 0x13 and len(payload) == 40
            partition, self.offset, self.size, ops = struct.unpack("<QQQI12x", payload)
            assert partition == 0 and ops == SFC_NOR_OPS
            self.requests.append(request)

        def control(self, request, size):
            if request == 0x10:
                return struct.pack("<i", 0)
            assert request == 0x19 and size == 8
            crc = crc_raw(self.data[self.offset:self.offset + self.size])
            return struct.pack("<iI", 0, crc ^ int(self.bad_crc))

        def receive(self, size):
            assert size == self.size
            return self.data[self.offset:self.offset + size]

    assert crc_raw(b"123456789") == 0x340bc6d9
    assert not ({0x11, 0x12} & ALLOWED_OUT)
    policy = ram_policy()
    assert struct.unpack_from("<I", policy, 28)[0] == 1
    assert struct.unpack_from("<II", policy, 172) == (0x44424700, 20)
    fake = FakeTransport()
    directory = ROOT / "build" / ("nor-backup-selftest-" + uuid.uuid4().hex)
    directory.mkdir(parents=True)
    paths = (directory / "one.bin", directory / "two.bin")
    try:
        first = read_pass(fake, paths[0], 1, False)
        second = read_pass(fake, paths[1], 2, False)
        verify_passes(first, second)
        assert first["sha256"] == digest(fake.data)
        assert len(fake.requests) == 2 * FLASH_BYTES // CHUNK_BYTES
        try:
            verify_passes(first, {**second, "sha256": "mismatch"})
        except RuntimeError:
            pass
        else:
            raise AssertionError("Mismatched passes were accepted")
        fake.bad_crc = True
        try:
            read_chunk(fake, 0, CHUNK_BYTES)
        except RuntimeError:
            pass
        else:
            raise AssertionError("Corrupted chunk was accepted")
    finally:
        for path in paths:
            path.unlink(missing_ok=True)
        directory.rmdir()
    print("PASS: read packet/CRC, double full-size persistence, SHA/length match, mismatch rejection; no USB accessed.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--execute", action="store_true", help="Explicitly open USB and run the read-only RAM helper backup")
    parser.add_argument("--self-test", action="store_true", help="Run fake-transport regression; no USB")
    parser.add_argument("--output", type=Path, help="New output directory (must not exist)")
    args = parser.parse_args()
    if args.self_test:
        if args.execute:
            parser.error("--self-test and --execute are mutually exclusive")
        self_test()
        return 0
    assets, asset_info = input_files()
    print("Inputs verified. Plan: stock RAM loader, JEDEC 0x852018, 2 x 16MiB reads, CRC + SHA256.")
    print("OUT allowlist:", ", ".join(f"0x{x:02x}" for x in sorted(ALLOWED_OUT)))
    if not args.execute:
        print("Dry run only: no USB opened and no backup files created. Use --execute only after entering USB Boot.")
        return 0
    destination = args.output or ROOT / "build/nor-backups" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    destination.mkdir(parents=True, exist_ok=False)
    metadata = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "status": "incomplete", "board": "HALLEY7-M-KIT X2600M DDR2 64MiB",
        "flash": "Puya PY25Q128H", "expected_jedec": "0x852018",
        "offset": 0, "length": FLASH_BYTES, "chunk_bytes": CHUNK_BYTES,
        "helper_profile": "CPU 800MHz DDR2 400MHz CL7, standard 0x03 / 24-bit NOR read",
        "inputs": asset_info, "passes": [], "forbidden_requests_sent": False,
    }
    metadata_path = destination / "metadata.json"

    def save_metadata():
        metadata_path.write_text(json.dumps(metadata, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    transport = None
    try:
        save_metadata()
        transport = UsbTransport()
        metadata["jedec"] = f"0x{load_helper(transport, assets):06x}"
        save_metadata()
        for number in (1, 2):
            metadata["passes"].append(read_pass(transport, destination / f"nor-pass{number}.bin", number))
            save_metadata()
        verify_passes(*metadata["passes"])
        (destination / "nor-pass1.bin").rename(destination / "nor-original.bin")
        metadata["passes"][0]["file"] = "nor-original.bin"
        metadata["status"] = "verified"
        print("Verified backup:", destination / "nor-original.bin")
        print("SHA256:", metadata["passes"][0]["sha256"])
        return 0
    except Exception as exc:
        metadata["status"] = "failed"
        metadata["error"] = str(exc)
        print("Backup failed:", exc)
        return 1
    finally:
        metadata["finished_utc"] = datetime.now(timezone.utc).isoformat()
        if transport is not None:
            metadata["out_requests_sent"] = sorted(set(transport.out_requests))
            try:
                transport.close()
            except Exception as exc:
                metadata["usb_close_error"] = str(exc)
        save_metadata()


if __name__ == "__main__":
    raise SystemExit(main())
