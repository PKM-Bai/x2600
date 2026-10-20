#!/usr/bin/env python3
"""Build production LVGL GUI with MSVC and capture headless PNGs; no SDL."""
import argparse
import array
import json
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]


def png(path, pixels, logical=False):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    payload = bytearray()
    for y in range(720):
        payload.append(0)
        for x in range(1280):
            color = pixels[y * 1280 + x] if logical else pixels[x * 720 + 719 - y]
            alpha = color >> 24
            # A synthetic video background makes transparent playback UI visible.
            background = (35 + x * 90 // 1280, 55 + y * 90 // 720, 85)
            for shift, back in zip((16, 8, 0), background):
                payload.append((((color >> shift) & 255) * alpha + back * (255 - alpha) + 127) // 255)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1280, 720, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(payload, 6)) + chunk(b"IEND", b""))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/player-gui-host")
    args = parser.parse_args()
    output = args.build.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shots = output / "screenshots"
    shots.mkdir(exist_ok=True)
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    cmake = shutil.which("cmake")
    if not cmake or not vcvars.is_file():
        raise SystemExit("CMake, Ninja and MSVC x64 are required")
    runner = output / "run-gui-tests.cmd"
    commands = ['set VSLANG=1033', f'call "{vcvars}" >nul',
                f'"{cmake}" -S "{ROOT / "projects/halley7m-player/gui"}" -B "{output}" -G Ninja -DPLAYER_GUI_HOST_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_DEBUG=/Od -DCMAKE_TRY_COMPILE_CONFIGURATION=Release',
                # Localized MSVC /showIncludes can disagree with Ninja's
                # encoding. Always rebuild this regression's host tree.
                f'"{cmake}" --build "{output}" --clean-first --parallel 4',
                f'"{output / "player_display_test.exe"}"',
                f'"{output / "player_display_format_test.exe"}"',
                f'"{output / "player_video_format_resolve_test.exe"}"',
                f'"{output / "player_rotator_test.exe"}"',
                f'"{output / "player_buttons_test.exe"}"',
                f'"{output / "player_buttons_callback_test.exe"}"',
                f'"{output / "player_gui_test.exe"}" "{shots}"']
    runner.write_text("@echo off\n" + "\n".join(c + '\nif not "%errorlevel%"=="0" exit /b 1' for c in commands) + "\nexit /b 0\n", encoding="utf-8")
    result = subprocess.run([os.environ.get("COMSPEC", "cmd.exe"), "/d", "/s", "/c", str(runner)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    (output / "test.log").write_text(result.stdout, encoding="utf-8")
    if result.returncode:
        lines = result.stdout.splitlines()
        errors = [line for line in lines if re.search(r"error [CL]|FAILED|Assertion|assertion|ninja:|fatal error", line)]
        print("\n".join(errors or lines[-30:]))
        raise SystemExit(result.returncode)
    if "PASS production LVGL GUI:" not in result.stdout:
        raise SystemExit("GUI test did not report successful completion; see test.log")
    if "PASS production display:" not in result.stdout:
        raise SystemExit("Display test did not report successful completion; see test.log")
    if "buttons regression PASS:" not in result.stdout or "PASS buttons callback:" not in result.stdout:
        raise SystemExit("Button tests did not report successful completion; see test.log")
    for line in result.stdout.splitlines():
        if line.startswith("PASS ") or line.startswith("buttons regression PASS:"):
            print(line)
    artifacts = []
    for name in ("library", "playback", "menu", "diagnostics", "audio-unknown", "playback-wide"):
        pixels = array.array("I")
        pixels.frombytes((shots / (name + ".argb")).read_bytes())
        if len(pixels) != 720 * 1280:
            raise ValueError("incomplete screenshot")
        destination = shots / (name + ".png")
        png(destination, pixels, logical=name == "playback-wide")
        artifacts.append(str(destination))
    print(json.dumps(dict(screenshots=artifacts)))


if __name__ == "__main__":
    main()
