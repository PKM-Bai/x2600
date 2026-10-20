"""Prepare/install the verified player through the board's existing U-Boot.

The default only checks local files and prepares an explicitly padded NOR
artifact. --execute uploads that artifact, verifies the previous installation,
programs NOR and saves bootcmd using flash-regba-nor's verified implementation.
The original compact RAM binary is never changed. No Cloner policy is used.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import time

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


flash = module("player_nor_shared", "flash-regba-nor.py")
verify = module("player_nor_elf", "verify-player.py")
require = flash.require


def identity(data):
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "crc32": f"{flash.crc(data):08x}"}


def previous_identity(data, record):
    actual = identity(data)
    require(record.get("status") == "programmed_and_verified" and
            record.get("offset") == flash.APP_OFFSET and
            all(record.get(key) == value for key, value in actual.items()) and
            record.get("bootcmd") == flash.boot_command(len(data)) and
            record.get("erase_bytes") == flash.erase_length(len(data)) and
            record.get("environment_crc_verified") is True and
            record.get("original_backup_sha256") == flash.BACKUP_SHA,
            "Previous BIN/installation record identity, bootcmd or erase range mismatch")
    return actual


def verified_backup():
    metadata = json.loads((flash.BACKUP / "metadata.json").read_text(encoding="utf-8"))
    require(metadata.get("status") == "verified" and metadata.get("length") == flash.FLASH_SIZE,
            "Original NOR backup is not verified")
    original = (flash.BACKUP / "nor-original.bin").read_bytes()
    second = (flash.BACKUP / "nor-pass2.bin").read_bytes()
    require(len(original) == flash.FLASH_SIZE and original == second and
            hashlib.sha256(original).hexdigest() == flash.BACKUP_SHA,
            "Two original NOR backups must match the known SHA256")
    require(struct.unpack_from("<I", original, 0x588c)[0] == flash.ERASE_SIZE,
            "Original NOR erase geometry changed")
    return original


def padded_image(source, erase_bytes, bss_start, bss_end):
    require(erase_bytes == flash.erase_length(erase_bytes) and len(source) <= erase_bytes,
            "Invalid padded NOR image size")
    # The linked file ends before the aligned BSS start. Its short alignment
    # gap is unused; the rest of this additional upload lies in BSS, cleared
    # by mips/start.S before platform_c_entry. No live initialized data moves.
    require(flash.ram.ADDRESS + len(source) <= bss_start <=
            flash.ram.ADDRESS + erase_bytes <= bss_end,
            "NOR padding must end within the entry-cleared BSS")
    return source + b"\xff" * (erase_bytes - len(source))


def write_matching(path, data):
    if path.exists():
        require(path.read_bytes() == data, f"Refusing to replace a different prepared artifact: {path}")
    else:
        path.write_bytes(data)


def prepare(source_path, source_manifest_path, previous_path, previous_record_path, output):
    source = source_path.read_bytes()
    flash.erase_length(len(source))
    manifest_bytes = source_manifest_path.read_bytes()
    source_manifest = json.loads(manifest_bytes)
    source_record = source_manifest.get("mips", {})
    source_id = identity(source)
    require(source_manifest.get("artifact_type") == "player-compact-package" and
            source_manifest.get("audio_default_volume_percent") == 7 and
            source_manifest.get("audio_enabled_at_boot") is False and
            source_record.get("entry") == "0x80600000" and
            source_record.get("binary_bytes") == source_id["bytes"] and
            source_record.get("binary_sha256") == source_id["sha256"] and
            source_record.get("binary_crc32") == source_id["crc32"],
            "Player source manifest, compact image identity, entry or default volume mismatch")
    elf_path = Path(source_record["elf"])
    elf = verify.Elf32(elf_path)
    elf.window(8, flash.ram.ADDRESS, verify.MIPS_BASE, verify.MIPS_END)
    elf.binary_matches(source, flash.ram.ADDRESS)
    require(elf.symbols.get("_start") == flash.ram.ADDRESS,
            "Player ELF entry does not use the standalone BSS-clearing startup")
    bss_start, bss_end = (elf.symbols.get(name, 0) for name in ("__bss_start", "__bss_end"))
    previous = previous_path.read_bytes()
    previous_record = json.loads(previous_record_path.read_text(encoding="utf-8"))
    previous_id = previous_identity(previous, previous_record)
    original = verified_backup()
    require(previous_record.get("bootloader_prefix_crc32") ==
            f"{flash.crc(original[:flash.ENV_OFFSET]):08x}",
            "Previous installation's preserved bootloader identity mismatch")
    erased = max(flash.erase_length(len(source)), previous_record["erase_bytes"])
    image = padded_image(source, erased, bss_start, bss_end)
    image_id = identity(image)
    package_manifest = {
        "artifact_type": "player-nor-padded", "binary": "halley7m_player_nor.bin",
        "binary_bytes": image_id["bytes"], "binary_sha256": image_id["sha256"],
        "binary_crc32": image_id["crc32"], "entry": "0x80600000",
        "audio_default_volume_percent": 7,
        "source": {"binary": str(source_path.resolve()), **source_id,
                   "elf": str(elf_path.resolve()), "elf_sha256": hashlib.sha256(elf.data).hexdigest(),
                   "manifest": str(source_manifest_path.resolve()),
                   "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest()},
        "padding": {"offset": len(source), "bytes": len(image) - len(source), "fill": "ff",
                    "reason": "retain_previous_32KiB_erase_extent"},
        "startup": {"bss_start": f"0x{bss_start:08x}", "bss_end": f"0x{bss_end:08x}",
                    "padding_end": f"0x{flash.ram.ADDRESS + len(image):08x}",
                    "cleared_by": "projects/halley7m-player/mips/start.S"},
        "previous": {"binary": str(previous_path.resolve()), **previous_id,
                     "installation_record": str(previous_record_path.resolve()),
                     "erase_bytes": previous_record["erase_bytes"]},
    }
    output.mkdir(parents=True, exist_ok=True)
    image_path = output / package_manifest["binary"]
    write_matching(image_path, image)
    write_matching(output / "manifest.json",
                   (json.dumps(package_manifest, indent=2) + "\n").encode("utf-8"))
    # Exercise the original installer's exact manifest/backup gate as well.
    checked_image, checked_original = flash.inputs(image_path)
    require(checked_image == image and checked_original == original,
            "Prepared NOR artifact failed the shared installer's input verification")
    return image_path, image, original, previous, package_manifest


def player_command(console, command, marker, timeout=5):
    console.log("TX " + command)
    console.write(command.encode("ascii") + b"\r")
    data = bytearray()
    deadline = time.monotonic() + timeout
    pattern = re.compile(rb"\b" + marker.encode("ascii") + rb" ([^\r\n]*)\r?\n")
    while time.monotonic() < deadline:
        chunk = console.serial.read(1)
        if chunk:
            data.extend(chunk)
            match = pattern.search(data)
            if match:
                console.log("RX " + bytes(data).decode("ascii", errors="backslashreplace"))
                return dict(re.findall(r"(?:^|\s)(\w+)=([^\s]+)", match[1].decode("ascii")))
    console.log("RX (timeout) " + bytes(data).decode("ascii", errors="backslashreplace"))
    raise RuntimeError(f"Player did not confirm {marker}; no reboot or NOR operation sent")


def stop_player(console):
    console.drain()
    player_command(console, "status", "AMP_STATUS")
    post = player_command(console, "stop", "SESSION_POST")
    require(post.get("kind") == "5" and post.get("result") == "0",
            "Player STOP was not accepted; no reboot sent")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        session = player_command(console, "session", "SESSION_STATUS")
        if session.get("request") == post.get("request"):
            require(all(session.get(key) == "0" for key in ("state", "kind", "pinned", "error", "result")),
                    "Player STOP did not release resources; no reboot sent")
            audio = player_command(console, "audio", "AUDIO_STATUS")
            require(audio.get("state") == "0" and audio.get("active") == "0",
                    "Player audio is still active; no reboot sent")
            return
        time.sleep(0.05)
    raise RuntimeError("Player STOP acknowledgement timed out; no reboot sent")


def self_test():
    flash.self_test()
    # Source bytes remain exact, old erase extent remains representable by the
    # shared expected_kernel() on this and every subsequent installation.
    source = b"player source" * 4000
    old = b"old application" * 6000
    original = b"\x5a" * flash.FLASH_SIZE
    erased = max(flash.erase_length(len(source)), flash.erase_length(len(old)))
    bss_start = flash.ram.ADDRESS + len(source) + 32
    bss_end = flash.ram.ADDRESS + erased + 4096
    image = padded_image(source, erased, bss_start, bss_end)
    require(image[:len(source)] == source and image[len(source):] == b"\xff" * (erased - len(source)),
            "Padded artifact changed source bytes or left a non-FF tail")
    kernel = flash.expected_kernel(original, image)
    require(kernel[:erased] == image and kernel[erased:] == original[flash.APP_OFFSET + erased:flash.ROOTFS_OFFSET],
            "Padded installation cannot be reconstructed as the next previous image")
    record = {"status": "programmed_and_verified", "offset": flash.APP_OFFSET,
              **identity(image), "bootcmd": flash.boot_command(len(image)),
              "erase_bytes": erased, "environment_crc_verified": True,
              "original_backup_sha256": flash.BACKUP_SHA}
    previous_identity(image, record)
    for key in ("bytes", "sha256", "crc32", "bootcmd", "erase_bytes", "environment_crc_verified"):
        corrupted = dict(record, **{key: None})
        try:
            previous_identity(image, corrupted)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"Accepted invalid previous installation {key}")
    for args in ((source, erased, bss_start, flash.ram.ADDRESS + erased - 1),
                 (source, erased - 1, bss_start, bss_end),
                 (source, flash.APP_CAPACITY + flash.ERASE_SIZE, bss_start, bss_end)):
        try:
            padded_image(*args)
        except RuntimeError:
            pass
        else:
            raise AssertionError("Accepted unsafe padding boundary")
    print("PASS: player NOR source/FF padding, BSS bounds, previous identity, repeat-install layout; no serial opened")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--execute", action="store_true")
    mode.add_argument("--self-test", action="store_true")
    parser.add_argument("--file", type=Path, default=ROOT / "build/player-riscv-Release/mips/halley7m_player.bin")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--previous-bin", type=Path)
    parser.add_argument("--previous-install", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/player-nor-install-20261020")
    parser.add_argument("--port", default="COM52")
    boot = parser.add_mutually_exclusive_group()
    boot.add_argument("--reboot-player", action="store_true")
    boot.add_argument("--wait-boot", action="store_true")
    parser.add_argument("--reuse-ram", action="store_true",
                        help="Reuse the identical verified padded upload; recheck its RAM CRC before erase")
    parser.add_argument("--log", type=Path, default=ROOT / "build/flash-player-nor-20261020.log")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    require(not args.reuse_ram or not (args.reboot_player or args.wait_boot),
            "--reuse-ram requires the existing U-Boot upload without any reset")
    require(args.previous_bin is not None and args.previous_install is not None,
            "Specify both --previous-bin and --previous-install for the current NOR installation")
    manifest_path = args.manifest or args.file.parent.parent / "manifest.json"
    image_path, image, original, previous, package = prepare(
        args.file, manifest_path, args.previous_bin, args.previous_install, args.output)
    plan = {"mode": "player_nor_padded_update", "execute": args.execute,
            "image": str(image_path.resolve()), **identity(image),
            "source": package["source"], "padding": package["padding"],
            "erase_offset": flash.APP_OFFSET, "erase_bytes": len(image),
            "bootcmd": flash.boot_command(len(image)), "environment_offset": flash.ENV_OFFSET,
            "previous": package["previous"], "original_backup_sha256": flash.BACKUP_SHA}
    print(json.dumps(plan, indent=2), flush=True)
    if not args.execute:
        return 0
    require(not args.log.exists() and not args.log.with_suffix(".json").exists(),
            "Use a fresh --log path; existing NOR evidence will not be overwritten")
    import serial
    log = flash.ram.Log(args.log)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=10,
                         xonxoff=False, rtscts=False, dsrdtr=False)
    port.dtr = port.rts = False
    port.port = args.port
    try:
        port.open()
        console = flash.ram.Console(port, log)
        if args.reboot_player:
            stop_player(console)
            console.reboot_player()
        elif args.wait_boot:
            console.wait_boot()
        else:
            console.drain()
        require(flash.ram.probe(console) == (1152000000, 1152000000, 1800000000, 600000000),
                "Unexpected normal U-Boot clocks; no NOR erase sent")
        for command in ("help sfcnor", "help saveenv", "help cmp", "help mw", "help reset"):
            console.command(command)
        log("PLAYER_NOR_PACKAGE " + json.dumps(plan, separators=(",", ":")))
        if args.reuse_ram:
            log("Reusing uploaded padded image; program() will recheck RAM CRC before any erase")
        else:
            flash.ram.upload(console, image_path, image, no_go=True)
        result = flash.program(console, image, original, previous)
        result.update(artifact_type="player-nor-padded", source=package["source"],
                      padding=package["padding"], manifest=str((args.output / "manifest.json").resolve()),
                      audio_default_volume_percent=7)
        args.log.with_suffix(".json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        log("TX reset (boot verified player NOR package using persisted environment)")
        console.write(b"reset\r")
        # Keep the UART startup bytes in this log. A later independent status
        # check establishes application startup; this is not a cold-boot claim.
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
