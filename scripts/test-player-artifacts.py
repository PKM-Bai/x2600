#!/usr/bin/env python3
"""Test production ELF/BIN verification using real player build artifacts.

From the repository use build/player-riscv-Release by default. From a player
build directory (or its mips/riscv child), detect that build instead. Explicit
--mips-build and --riscv-build paths override discovery. Corrupt copies and a
result report remain under build/temp; original firmware is never written.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("verify_player_production", ROOT / "scripts/verify-player.py")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


def remote_build(build_root):
    for name in ("riscv-wsl", "riscv"):
        candidate = build_root / name
        if (candidate / "player_riscv.elf").is_file():
            return candidate
    return build_root / "riscv-wsl"


def discover(args):
    if args.mips_build or args.riscv_build:
        build_root = (args.mips_build or args.riscv_build).resolve().parent
    else:
        build_root = ROOT / "build/player-riscv-Release"
        for candidate in (Path.cwd(), Path.cwd().parent):
            if ((candidate / "mips/halley7m_player.elf").is_file() and
                    (remote_build(candidate) / "player_riscv.elf").is_file()):
                build_root = candidate
                break
    return ((args.mips_build or build_root / "mips").resolve(),
            (args.riscv_build or remote_build(build_root)).resolve())


def first_load_offset(data):
    phoff = struct.unpack_from("<I", data, 28)[0]
    size, count = struct.unpack_from("<HH", data, 42)
    for index in range(count):
        offset = phoff + size * index
        kind, _, _, _, file_bytes, memory_bytes = struct.unpack_from("<IIIIII", data, offset)
        if kind == 1 and file_bytes and memory_bytes:
            return offset
    raise AssertionError("valid fixture needs a nonempty loadable segment")


def check_core(name, build, machine, entry, start, end, temporary):
    stem = "halley7m_player" if name == "mips" else "player_riscv"
    elf_path, binary_path = build / (stem + ".elf"), build / (stem + ".bin")
    elf = VERIFY.Elf32(elf_path)
    elf.window(machine, entry, start, end)
    binary = binary_path.read_bytes()
    elf.binary_matches(binary, start)
    original_elf = elf.data
    original_hashes = (hashlib.sha256(original_elf).hexdigest(), hashlib.sha256(binary).hexdigest())
    print(f"PASS {name}: real ELF architecture/entry/ABI/window and exact BIN bytes")
    directory = temporary / name
    directory.mkdir()
    rejected = []

    def reject(label, operation, expected):
        try:
            operation()
        except RuntimeError as error:
            if expected not in str(error):
                raise AssertionError(f"{name}/{label}: wrong rejection: {error}") from error
            rejected.append(dict(case=label, reason=str(error)))
            print(f"PASS {name}: reject {label}")
        else:
            raise AssertionError(f"{name}/{label}: production verifier accepted corrupt artifact")

    def corrupt_elf(label, offset, fmt, value, expected):
        data = bytearray(original_elf)
        struct.pack_into(fmt, data, offset, value)
        path = directory / (label + ".elf")
        path.write_bytes(data)
        reject(label, lambda: VERIFY.Elf32(path).window(machine, entry, start, end), expected)

    corrupt_elf("wrong-architecture", 18, "<H", 8 if machine == 243 else 243,
                "wrong architecture or entry")
    corrupt_elf("wrong-entry", 24, "<I", entry + 4, "wrong architecture or entry")
    load = first_load_offset(original_elf)
    corrupt_elf("load-vaddr-above-window", load + 8, "<I", end, "escapes private DDR")
    corrupt_elf("load-paddr-below-window", load + 12, "<I", start - 1, "escapes private DDR")
    vaddr = struct.unpack_from("<I", original_elf, load + 8)[0]
    corrupt_elf("load-size-crosses-window", load + 20, "<I", end - vaddr + 1,
                "escapes private DDR")
    if name == "riscv":
        corrupt_elf("rve-abi", 36, "<I", elf.flags | 0x8, "not RVE")

    # Corrupt an actual PT_LOAD byte, rather than a padding byte or ELF header.
    segment_paddr = struct.unpack_from("<I", original_elf, load + 12)[0]
    segment_size = struct.unpack_from("<I", original_elf, load + 16)[0]
    changed = bytearray(binary)
    changed[segment_paddr - start + segment_size // 2] ^= 0x80
    changed_path = directory / "changed-byte.bin"
    changed_path.write_bytes(changed)
    reject("changed-bin-byte", lambda: elf.binary_matches(changed_path.read_bytes(), start),
           "binary differs from ELF load bytes")
    truncated_path = directory / "truncated.bin"
    truncated_path.write_bytes(binary[:-1])
    reject("truncated-bin", lambda: elf.binary_matches(truncated_path.read_bytes(), start),
           "binary differs from ELF load bytes")
    if (hashlib.sha256(elf_path.read_bytes()).hexdigest(),
            hashlib.sha256(binary_path.read_bytes()).hexdigest()) != original_hashes:
        raise AssertionError(f"{name}: original artifacts changed while tests were running")
    return dict(build=str(build), binary_bytes=len(binary), elf_sha256=original_hashes[0],
                binary_sha256=original_hashes[1], accepted_real_artifact=True,
                rejected=rejected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mips-build", type=Path)
    parser.add_argument("--riscv-build", type=Path)
    args = parser.parse_args()
    mips, riscv = discover(args)
    parent = ROOT / "build/temp"
    parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="player-artifacts-", dir=parent))
    results = {
        "mips": check_core("mips", mips, 8, VERIFY.MIPS_BASE, VERIFY.MIPS_BASE, VERIFY.MIPS_END, temporary),
        "riscv": check_core("riscv", riscv, 243, VERIFY.RISCV_ENTRY,
                            VERIFY.RISCV_BASE, VERIFY.RISCV_END, temporary),
    }
    rejected = sum(len(item["rejected"]) for item in results.values())
    report = dict(production_verifier=str(ROOT / "scripts/verify-player.py"),
                  positive_cases=2, rejected_cases=rejected, artifacts=results,
                  hardware_accessed=False, temporary=str(temporary))
    (temporary / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(passed=2 + rejected, temporary=str(temporary))))


if __name__ == "__main__":
    main()
