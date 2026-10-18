"""Install this board's ReGBA raw image using stock U-Boot on COM52.

Default is a local-only plan. --execute updates the beginning of the kernel
partition and saves bootcmd; SPL/U-Boot code and rootfs are checked unchanged.
The original verified 16MiB backup is required. No Cloner policy is used.
--previous identifies an already installed raw image; its complete expected
kernel layout and persisted boot environment must match before any erase.
Updates support the same or a larger 32KiB-aligned erase range only.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("ramload", ROOT / "scripts/ramload-regba.py")
ram = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ram)
APP_OFFSET, APP_CAPACITY = 0x40000, 0x600000
ENV_OFFSET, ENV_SIZE = 0x3f000, 0x1000
ROOTFS_OFFSET, FLASH_SIZE, ERASE_SIZE = 0x640000, 0x1000000, 0x8000
# Keep readback above the entire supported 6MiB upload window. Even the
# largest rootfs read ends below U-Boot's 0x82f67000 malloc reservation.
SCRATCH = 0x81200000
BACKUP = ROOT / "build/nor-backups/20261016T024011Z"
BACKUP_SHA = "5efd956157cf88fc8a55dd3695f75b216e184a9fa15bee00149680c740077926"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def crc(data):
    return zlib.crc32(data) & 0xffffffff


def boot_command(size):
    return f"sfcnor read 0x40000 0x{size:x} 0x80600000;go 0x80600000"


def erase_length(size):
    require(0 < size <= APP_CAPACITY, "Image must fit the fixed 6MiB kernel partition")
    return (size + ERASE_SIZE - 1) // ERASE_SIZE * ERASE_SIZE


def expected_kernel(original, previous=None):
    kernel = bytearray(original[APP_OFFSET:APP_OFFSET + APP_CAPACITY])
    if previous is not None:
        erased = erase_length(len(previous))
        kernel[:erased] = previous + b"\xff" * (erased - len(previous))
    return bytes(kernel)


def inputs(path):
    image = path.read_bytes()
    erase_length(len(image))
    manifest = json.loads(path.with_name("manifest.json").read_text(encoding="utf-8"))
    require(manifest["binary_bytes"] == len(image) and
            manifest["binary_sha256"] == hashlib.sha256(image).hexdigest() and
            manifest["entry"] == "0x80600000", "Build manifest does not match image/entry")
    metadata = json.loads((BACKUP / "metadata.json").read_text())
    require(metadata["status"] == "verified" and metadata["length"] == FLASH_SIZE,
            "Original NOR backup is not verified")
    original = (BACKUP / "nor-original.bin").read_bytes()
    second = (BACKUP / "nor-pass2.bin").read_bytes()
    require(len(original) == FLASH_SIZE and original == second and
            hashlib.sha256(original).hexdigest() == BACKUP_SHA,
            "Two persisted original NOR backups must match the known SHA256")
    require(struct.unpack_from("<I", original, 0x588c)[0] == ERASE_SIZE,
            "Original NOR erase geometry changed")
    return image, original


def ram_crc(console, address, size):
    output = console.command(f"crc32 {address:x} {size:x}", 30)
    values = re.findall(rb"CRC32 for [^\r\n]*?==>\s*([0-9a-fA-F]{8})", output)
    require(len(values) == 1, "Missing/ambiguous U-Boot CRC32 response")
    return int(values[0], 16)


def read_crc(console, offset, size):
    require(0 <= offset < FLASH_SIZE and 0 < size <= FLASH_SIZE - offset,
            "NOR read out of bounds")
    # sfcnor prints success even when its underlying operation fails. Poison
    # the destination so a failed read cannot validate stale scratch contents.
    console.command(f"mw.b {SCRATCH:x} a5 {size:x}", 30)
    console.command(f"sfcnor read {offset:x} {size:x} {SCRATCH:x}", 60)
    return ram_crc(console, SCRATCH, size)


def parse_dump(output, address, size):
    result = bytearray()
    for line in output.splitlines():
        match = re.match(rb"^([0-9a-fA-F]{8}):\s+((?:[0-9a-fA-F]{2}\s+){16})", line)
        if match:
            require(int(match[1], 16) == address + len(result), "Discontinuous memory dump")
            result.extend(bytes.fromhex(match[2].decode()))
    require(len(result) == size, "Incomplete environment dump")
    return bytes(result)


def verify_environment(data, command):
    require(len(data) == ENV_SIZE and struct.unpack_from("<I", data)[0] == crc(data[4:]),
            "Persisted environment CRC mismatch")
    require(b"\0\0" in data[4:], "Environment lacks terminator")
    entries = dict(entry.split(b"=", 1) for entry in data[4:].split(b"\0\0", 1)[0].split(b"\0"))
    require(entries.get(b"bootcmd") == command.encode() and entries.get(b"bootdelay") == b"1",
            "Persisted bootcmd/bootdelay mismatch")
    return {key.decode(): value.decode() for key, value in entries.items()}


def read_environment(console, command):
    read_crc(console, ENV_OFFSET, ENV_SIZE)
    # Keep each UART response below the Windows serial input buffer. A full
    # 4KiB hex dump is over 20KiB and can lose rows while reading the prompt.
    data = bytearray()
    for offset in range(0, ENV_SIZE, 0x100):
        address = SCRATCH + offset
        dump = console.command(f"md.b {address:x} 100", 15)
        data.extend(parse_dump(dump, address, 0x100))
    return verify_environment(bytes(data), command)


def program(console, image, original, previous=None):
    size, erased = len(image), erase_length(len(image))
    expected = expected_kernel(original, previous)
    if previous is not None:
        require(erased >= erase_length(len(previous)),
                "Update erase range cannot shrink; the previous tail would remain in NOR")
    require(ram_crc(console, ram.ADDRESS, size) == crc(image), "RAM image changed before programming")
    # The bootloader always matches the original backup. A first installation
    # requires its original environment/kernel; an update requires the exact
    # previous raw image over that original kernel and its valid environment.
    # Linux may have changed rootfs since that backup; preserve its current
    # contents by comparing the same region before and after this operation.
    regions = [(0, ENV_OFFSET)]
    if previous is None:
        regions.append((ENV_OFFSET, ENV_SIZE))
    for offset, length in regions:
        require(read_crc(console, offset, length) == crc(original[offset:offset + length]),
                f"Current NOR differs from original backup at {offset:#x}; no erase sent")
    if previous is not None:
        read_environment(console, boot_command(len(previous)))
    require(read_crc(console, APP_OFFSET, APP_CAPACITY) == crc(expected),
            "Current NOR kernel differs from the expected previous image/original backup; no erase sent")
    rootfs_crc = read_crc(console, ROOTFS_OFFSET, FLASH_SIZE - ROOTFS_OFFSET)
    console.log(f"PRESERVE current rootfs CRC32={rootfs_crc:08x}; original backup CRC32={crc(original[ROOTFS_OFFSET:]):08x}")
    console.command(f"sfcnor erase {APP_OFFSET:x} {erased:x}", 60)
    require(read_crc(console, APP_OFFSET, erased) == crc(b"\xff" * erased),
            "Kernel erase readback failed; no write or environment save sent")
    console.command(f"sfcnor write {APP_OFFSET:x} {size:x} {ram.ADDRESS:x} 0", 60)
    require(read_crc(console, APP_OFFSET, erased) == crc(image + b"\xff" * (erased - size)),
            "Programmed kernel readback CRC mismatch; bootcmd was not changed")
    # Also compare every actual program byte, rather than relying only on CRC.
    output = console.command(f"cmp.b {ram.ADDRESS:x} {SCRATCH:x} {size:x}", 30)
    require(re.search(rb"Total of\s+" + str(size).encode() + rb"\s+byte\(s\) were the same", output),
            "Programmed kernel byte comparison failed; bootcmd was not changed")
    command = boot_command(size)
    console.command("setenv bootcmd '" + command + "'")
    console.command("setenv bootdelay 1")
    console.command("saveenv", 60)
    environment = read_environment(console, command)
    for offset, length, expected in ((0, ENV_OFFSET, crc(original[:ENV_OFFSET])),
                                     (ROOTFS_OFFSET, FLASH_SIZE - ROOTFS_OFFSET, rootfs_crc)):
        require(read_crc(console, offset, length) == expected,
                f"Protected NOR region changed at {offset:#x}; do not reset")
    console.log("VERIFIED NOR program bytes, environment CRC/bootcmd, unchanged SPL/U-Boot code and rootfs")
    return {"status": "programmed_and_verified", "offset": APP_OFFSET, "bytes": size,
            "erase_bytes": erased, "crc32": f"{crc(image):08x}",
            "sha256": hashlib.sha256(image).hexdigest(), "bootcmd": environment["bootcmd"],
            "previous_bytes": len(previous) if previous is not None else None,
            "previous_crc32": f"{crc(previous):08x}" if previous is not None else None,
            "previous_sha256": hashlib.sha256(previous).hexdigest() if previous is not None else None,
            "environment_crc_verified": True,
            "bootloader_prefix_crc32": f"{crc(original[:ENV_OFFSET]):08x}",
            "rootfs_crc32": f"{rootfs_crc:08x}",
            "original_backup": str(BACKUP / "nor-original.bin"),
            "original_backup_sha256": BACKUP_SHA, "cold_boot_verified": False}


def self_test():
    # Exercise the exact programming workflow, including ignored media errors,
    # with a memory-backed console. No serial or USB import/open occurs.
    original = b"\x5a" * FLASH_SIZE
    image = bytes(range(256)) * 401 + b"end"
    require(ram.ADDRESS + APP_CAPACITY <= SCRATCH and
            SCRATCH + FLASH_SIZE - ROOTFS_OFFSET < 0x82f67000,
            "NOR scratch overlaps the supported image or U-Boot heap")

    def environment(command, invalid_crc=False):
        body = (b"bootcmd=" + command.encode() + b"\0bootdelay=1\0\0").ljust(ENV_SIZE - 4, b"\0")
        return struct.pack("<I", crc(body) ^ int(invalid_crc)) + body

    class Fake:
        def __init__(self, failure="", installed=None):
            self.nor = bytearray(original)
            if installed is not None:
                self.nor[APP_OFFSET:APP_OFFSET + APP_CAPACITY] = expected_kernel(original, installed)
                self.nor[ENV_OFFSET:ENV_OFFSET + ENV_SIZE] = environment(
                    boot_command(len(installed)), failure == "previous_env")
            # A pre-existing Linux filesystem change must survive install.
            self.nor[ROOTFS_OFFSET + 17] ^= 0x80
            self.scratch = b""
            self.commands = []
            self.failure = failure
            self.command_value = ""

        def log(self, message):
            pass

        def command(self, text, timeout=15):
            self.commands.append(text)
            words = text.split()
            if words[0] == "crc32":
                address, size = (int(x, 16) for x in words[1:])
                value = crc(image[:size] if address == ram.ADDRESS else self.scratch[:size])
                if self.failure == "pristine" and len(self.commands) == 4:
                    value ^= 1
                return f"CRC32 for {address:x} ... ==> {value:08x}\n".encode()
            if words[0] == "mw.b":
                self.scratch = bytes((int(words[2], 16),)) * int(words[3], 16)
            elif words[:2] == ["sfcnor", "read"]:
                offset, size = (int(x, 16) for x in words[2:4])
                self.scratch = bytes(self.nor[offset:offset + size])
            elif words[:2] == ["sfcnor", "erase"]:
                offset, size = (int(x, 16) for x in words[2:4])
                require(offset == APP_OFFSET and size == erase_length(len(image)), "Wrong erase range")
                if self.failure != "erase": self.nor[offset:offset + size] = b"\xff" * size
            elif words[:2] == ["sfcnor", "write"]:
                offset, size, address, erase = (int(x, 16) for x in words[2:])
                require(offset == APP_OFFSET and address == ram.ADDRESS and erase == 0 and size == len(image), "Wrong write range")
                if self.failure != "write": self.nor[offset:offset + size] = image
            elif words[0] == "cmp.b":
                return f"Total of {len(image)} byte(s) were the same\n".encode()
            elif words[:2] == ["setenv", "bootcmd"]:
                self.command_value = text.split("'", 2)[1]
            elif words[0] == "saveenv":
                self.nor[ENV_OFFSET:ENV_OFFSET + ENV_SIZE] = environment(
                    self.command_value, self.failure == "env")
                if self.failure == "protected": self.nor[ROOTFS_OFFSET + 17] ^= 1
            elif words[0] == "md.b":
                address, size = (int(x, 16) for x in words[1:])
                require(0 < size <= 0x100, "Environment response exceeds the verified UART chunk size")
                offset = address - SCRATCH
                return b"\n".join(f"{address + i:08x}: ".encode() + self.scratch[offset+i:offset+i+16].hex(" ").encode() + b"  ................" for i in range(0, size, 16))
            return b"ok\n"

    for failure in ("", "pristine", "erase", "write", "env", "protected"):
        console = Fake(failure)
        try:
            program(console, image, original)
        except RuntimeError:
            require(bool(failure), "Valid programming model failed")
            if failure == "pristine": require(not any(c.startswith("sfcnor erase") for c in console.commands), "Erased after bad original CRC")
            if failure in ("erase", "write"): require("saveenv" not in console.commands, "Saved bootcmd after failed media operation")
        else:
            require(not failure, f"Ignored {failure} failure")
    previous = b"installed version" * 2048 + b"old end"
    console = Fake(installed=previous)
    result = program(console, image, original, previous)
    require(result["previous_bytes"] == len(previous) and
            result["previous_crc32"] == f"{crc(previous):08x}" and
            result["previous_sha256"] == hashlib.sha256(previous).hexdigest(),
            "Update result lost the verified previous-image identity")
    require(bytes(console.nor[APP_OFFSET:APP_OFFSET + APP_CAPACITY]) == expected_kernel(original, image),
            "Update did not preserve the expected complete kernel layout")
    for failure in ("previous_image", "previous_env"):
        console = Fake(failure=failure, installed=previous)
        claimed_previous = bytes((previous[0] ^ 1,)) + previous[1:] if failure == "previous_image" else previous
        try:
            program(console, image, original, claimed_previous)
        except RuntimeError:
            require(not any(c.startswith("sfcnor erase") for c in console.commands) and
                    "saveenv" not in console.commands,
                    "Erased/saved after a previous image or environment mismatch")
        else:
            raise AssertionError(f"Accepted mismatched {failure}")
    larger_previous = b"old" * (ERASE_SIZE * 2)
    console = Fake(installed=larger_previous)
    try:
        program(console, image, original, larger_previous)
    except RuntimeError:
        require(not console.commands, "Shrinking update reached hardware commands")
    else:
        raise AssertionError("Accepted a shrinking update erase range")
    for size in (0, APP_CAPACITY + 1):
        try: erase_length(size)
        except RuntimeError: pass
        else: raise AssertionError("Accepted image outside kernel partition")
    print("PASS: NOR first install/update, complete previous kernel/environment checks, erase/program/byte readback, full environment CRC, protected regions and failure stops; no hardware")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--execute", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    parser.add_argument("--file", type=Path, default=ROOT / "build/regba-Release/halley7m_regba.bin")
    parser.add_argument("--previous", type=Path,
                        help="Previously installed raw image; verify its kernel/environment. Updates require the same or a larger 32KiB-aligned erase range")
    parser.add_argument("--port", default="COM52")
    parser.add_argument("--wait-boot", action="store_true")
    parser.add_argument("--reuse-ram", action="store_true",
                        help="Reuse a previous verified upload; check its RAM CRC again before erase")
    parser.add_argument("--log", type=Path, default=ROOT / "build/flash-regba-nor-20261018.log")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    image, original = inputs(args.file)
    previous = args.previous.read_bytes() if args.previous is not None else None
    if previous is not None:
        require(erase_length(len(image)) >= erase_length(len(previous)),
                "Update erase range cannot shrink; the previous tail would remain in NOR")
    print(json.dumps({"mode": "update_nonshrinking" if previous is not None else "first_install",
                      "image": str(args.file), "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
                      "previous": {"image": str(args.previous), "bytes": len(previous),
                                   "erase_bytes": erase_length(len(previous)),
                                   "sha256": hashlib.sha256(previous).hexdigest(),
                                   "crc32": f"{crc(previous):08x}"} if previous is not None else None,
                      "erase_offset": APP_OFFSET, "erase_bytes": erase_length(len(image)),
                      "bootcmd": boot_command(len(image)), "environment_offset": ENV_OFFSET,
                      "backup": str(BACKUP), "execute": args.execute}, indent=2), flush=True)
    if not args.execute:
        return 0
    import serial
    log = ram.Log(args.log)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=10,
                         xonxoff=False, rtscts=False, dsrdtr=False)
    port.dtr = port.rts = False
    port.port = args.port
    try:
        port.open()
        console = ram.Console(port, log)
        if args.wait_boot: console.wait_boot()
        else: console.drain()
        require(ram.probe(console) == (1152000000, 1152000000, 1800000000, 600000000), "Unexpected normal boot clocks")
        for command in ("help sfcnor", "help saveenv", "help cmp", "help mw", "help reset"):
            console.command(command)
        if not args.reuse_ram:
            ram.upload(console, args.file, image, no_go=True)
        result = program(console, image, original, previous)
        args.log.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n")
        log("TX reset (boot verified NOR firmware using persisted environment)")
        console.write(b"reset\r")
        console.drain(quiet=3, limit=20)
        return 0
    except Exception as exc:
        log("ERROR: " + str(exc))
        return 1
    finally:
        port.close()
        log.close()


if __name__ == "__main__":
    raise SystemExit(main())
