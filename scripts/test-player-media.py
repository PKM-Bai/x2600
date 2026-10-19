#!/usr/bin/env python3
"""Build the portable player parser and exercise real RIFF/AVIX file layouts."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_player_media", ROOT / "scripts/prepare-player-media.py")
MEDIA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MEDIA)
PCM = struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16)
FIRST = b"\x00\x00\x00\x01\x09\xf0\x00\x00\x01\x67\x42\x00\x1f\x80\x00\x00\x01\x68\x80\x00\x00\x01\x65\x80"
NEXT = b"\x00\x00\x01\x09\xf0\x00\x00\x01\x41\x80"


def chunk(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack("<I", len(payload)) + payload + b"\0" * (len(payload) & 1)


def listing(tag: bytes, payload: bytes) -> bytes:
    return chunk(b"LIST", tag + payload)


def riff(tag: bytes, payload: bytes) -> bytes:
    return chunk(b"RIFF", tag + payload)


def stream(kind: bytes, count: int, fmt: bytes) -> bytes:
    header = bytearray(56)
    header[:4] = kind
    struct.pack_into("<II", header, 20, 1, 30 if kind == b"vids" else 48000)
    struct.pack_into("<I", header, 32, count)
    struct.pack_into("<I", header, 44, 0 if kind == b"vids" else 2)
    return listing(b"strl", chunk(b"strh", header) + chunk(b"strf", fmt))


def hdrl(frames=2, samples=3200, audio_first=False) -> bytes:
    bitmap = struct.pack("<IiiHH4sIiiII", 40, 720, 1280, 1, 24, b"H264", 0, 0, 0, 0, 0)
    video, audio = stream(b"vids", frames, bitmap), stream(b"auds", samples, PCM)
    avih = bytearray(56)
    struct.pack_into("<I", avih, 16, 1)  # Deliberately first-RIFF count, not full duration.
    return listing(b"hdrl", chunk(b"avih", avih) + (audio + video if audio_first else video + audio))


def avi(audio_first=False, au=FIRST, frames=2) -> bytes:
    vt, at = (b"01dc", b"00wb") if audio_first else (b"00dc", b"01wb")
    first = riff(b"AVI ", hdrl(frames=frames, audio_first=audio_first) + listing(b"movi", chunk(b"JUNK", b"abc") +
                 listing(b"rec ", chunk(vt, au) + chunk(at, b"\0" * 3200))) + chunk(b"idx1", b"ignored"))
    second = riff(b"AVIX", listing(b"movi", chunk(b"ix00", b"index") + chunk(vt, NEXT) +
                  chunk(at, b"\0" * 3200)))
    return first + second


def sparse_file(path: Path, parts: list[bytes | int]) -> None:
    with path.open("wb") as handle:
        if os.name == "nt":
            import ctypes
            import msvcrt
            returned = ctypes.c_ulong()
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
                                               ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                               ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
            if not kernel.DeviceIoControl(msvcrt.get_osfhandle(handle.fileno()), 0x900C4,
                                          None, 0, None, 0, ctypes.byref(returned), None):
                raise OSError(ctypes.get_last_error(), "cannot create sparse AVI test file")
        for part in parts:
            if isinstance(part, int):
                handle.seek(part, 1)
            else:
                handle.write(part)


def msvc_environment() -> dict:
    if shutil.which("cl"):
        return os.environ.copy()
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = ""
    if vswhere.exists():
        installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                                "-property", "installationPath"], text=True).strip()
    if not installation:
        installation = "C:/Program Files/Microsoft Visual Studio/18/Community"
    setup = Path(installation) / "VC/Auxiliary/Build/vcvars64.bat"
    if not setup.is_file():
        raise ValueError("MSVC not found; pass --cc with a working C compiler")
    lines = subprocess.check_output(f'call "{setup}" >nul && set', shell=True, text=True).splitlines()
    return dict(line.split("=", 1) for line in lines if "=" in line and not line.startswith("="))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cl" if os.name == "nt" else "gcc"))
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--skip-long", action="store_true")
    args = parser.parse_args()
    output = ROOT / "build/player-host"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ("player_media_test.exe" if os.name == "nt" else "player_media_test")
    source = ROOT / "projects/halley7m-player/common"
    if Path(args.cc).stem.lower() == "cl":
        environment = msvc_environment()
        compiler = shutil.which(args.cc, path=environment.get("Path", environment.get("PATH"))) or args.cc
        subprocess.run([compiler, "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/utf-8",
                        "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(source), str(source / "player_media.c"),
                        str(ROOT / "tests/player_media_test.c"), "/Fe:" + str(executable)],
                       cwd=output, env=environment, check=True)
    else:
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-D_FILE_OFFSET_BITS=64",
                        "-D_POSIX_C_SOURCE=200809L", "-I", str(source), str(source / "player_media.c"),
                        str(ROOT / "tests/player_media_test.c"), "-o", str(executable)], check=True)
    passed = 0

    def check(name: str, data: bytes | None, valid: bool = True):
        nonlocal passed
        path = output / name
        if data is not None:
            path.write_bytes(data)
        result = subprocess.run([str(executable), str(path)], capture_output=True, text=True)
        if (result.returncode == 0) != valid:
            raise AssertionError(f"{name}: {result.stdout}{result.stderr}")
        try:
            stats = MEDIA.validate_media(path)
        except ValueError:
            if valid:
                raise
        else:
            if not valid:
                raise AssertionError(f"Python scanner accepted {name}")
            if stats["kind"] == "AVI":
                assert stats["video_frames"] == 2 and stats["audio_samples"] == 3200
        passed += 1
        print(f"PASS {name}")

    check("normal.avi", avi())
    check("audio-first.avi", avi(audio_first=True))
    check("pcm.wav", riff(b"WAVE", chunk(b"JUNK", b"abc") + chunk(b"fmt ", PCM) + chunk(b"data", b"\0" * 5000)))
    check("truncated.avi", avi()[:-1], False)
    check("missing-sps.avi", avi(au=NEXT), False)
    check("two-aud.avi", avi(au=FIRST + NEXT), False)
    check("wrong-strh-length.avi", avi(frames=3), False)
    check("oversize.avi", avi(au=FIRST + b"\x55" * MEDIA.VIDEO_MAX), False)
    check("bad-pcm.wav", riff(b"WAVE", chunk(b"fmt ", PCM) + chunk(b"data", b"\0" * 3)), False)
    if not args.skip_long:
        # Three independently sized RIFFs cross both 1 GiB and signed 2 GiB offsets.
        parts = []
        for index in range(2):
            prefix = hdrl() if index == 0 else b""
            media = listing(b"movi", chunk(b"00dc", FIRST) + chunk(b"01wb", b"\0" * 3200)) if index == 0 else b""
            hole = (1 << 30) - 16 - len(prefix) - len(media)
            payload_size = 4 + len(prefix) + 8 + hole + len(media)
            parts += [b"RIFF" + struct.pack("<I", payload_size) + (b"AVI " if index == 0 else b"AVIX"),
                      prefix, b"JUNK" + struct.pack("<I", hole), hole, media]
        parts += [riff(b"AVIX", listing(b"movi", chunk(b"00dc", NEXT) + chunk(b"01wb", b"\0" * 3200)))]
        sparse_file(output / "over-2gib.avi", parts)
        check("over-2gib.avi", None)
    if args.ffmpeg:
        ffmpeg = args.ffmpeg.resolve()

        def check_converted(name: str, source: Path) -> tuple[Path, dict]:
            nonlocal passed
            prepared = output / name
            MEDIA.convert(source, prepared, ffmpeg, True)
            stats = MEDIA.validate_media(prepared)
            MEDIA.verify_codecs(prepared, ffmpeg)
            result = subprocess.run([str(executable), str(prepared)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(f"{name}: {result.stderr}")
            native = json.loads(result.stdout)
            assert native["frames"] == stats["video_frames"]
            assert native["samples"] == stats["audio_samples"]
            passed += 1
            return prepared, stats

        source_video = output / "source-silent.avi"
        subprocess.run([str(ffmpeg), "-v", "error", "-y", "-f", "lavfi", "-i",
                        "testsrc2=size=320x180:rate=30:duration=0.4", "-c:v", "rawvideo",
                        str(source_video)], check=True)
        check_converted("prepared.avi", source_video)
        print("PASS FFmpeg silent-input conversion and native-parser playback scan")

        # 720x576 at SAR 16:15 is a 4:3 picture. It must occupy 960x720,
        # centered within the 1280x720 display, rather than a squashed 900x720.
        sar_source = output / "source-sar.mkv"
        subprocess.run([str(ffmpeg), "-v", "error", "-y", "-f", "lavfi", "-i",
                        "color=red:size=720x576:rate=30:duration=0.4,setsar=16/15",
                        "-c:v", "ffv1", str(sar_source)], check=True)
        sar_stream = next(stream for stream in MEDIA.probe(sar_source, ffmpeg)["streams"]
                          if stream["codec_type"] == "video")
        assert sar_stream["sample_aspect_ratio"] == "16:15", "SAR fixture was not preserved"
        prepared, _ = check_converted("prepared-sar.avi", sar_source)
        decoded = subprocess.run([str(ffmpeg), "-v", "error", "-i", str(prepared),
                                  "-map", "0:v:0", "-vf", "transpose=cclock", "-frames:v", "1",
                                  "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"],
                                 check=True, capture_output=True).stdout
        assert len(decoded) == 1280 * 720 * 3, "unexpected decoded display size"
        row = decoded[360 * 1280 * 3:361 * 1280 * 3]
        red = [x for x in range(1280) if row[3*x] > 150 and
               row[3*x + 1] < 80 and row[3*x + 2] < 80]
        assert red and abs(red[0] - 160) <= 4 and abs(red[-1] - 1119) <= 4, \
            f"non-square pixels changed picture width: {red[0] if red else None}..{red[-1] if red else None}"
        print("PASS FFmpeg SAR16:15 keeps a centered 960x720 picture")

        # Video begins at 2s, while audio covers 0..3s. A video-only PTS reset
        # would incorrectly shorten the result to 1s and discard two seconds.
        delayed_source = output / "source-delayed-video.mkv"
        subprocess.run([str(ffmpeg), "-v", "error", "-y", "-f", "lavfi", "-i",
                        "testsrc2=size=320x180:rate=30:duration=1", "-f", "lavfi", "-i",
                        "sine=frequency=1000:sample_rate=48000:duration=3", "-filter_complex",
                        "[0:v]setpts=PTS+2/TB[v]", "-map", "[v]", "-map", "1:a:0",
                        "-c:v", "ffv1", "-fps_mode", "passthrough", "-c:a", "pcm_s16le",
                        str(delayed_source)], check=True)
        delayed_streams = MEDIA.probe(delayed_source, ffmpeg)["streams"]
        video_start = float(next(stream["start_time"] for stream in delayed_streams
                                 if stream["codec_type"] == "video"))
        audio_start = float(next(stream["start_time"] for stream in delayed_streams
                                 if stream["codec_type"] == "audio"))
        assert abs(video_start - audio_start - 2) < 0.05, "delayed-video fixture lost its offset"
        _, stats = check_converted("prepared-delayed-video.avi", delayed_source)
        assert stats["video_frames"] == 90, f"lost video timeline: {stats}"
        assert abs(stats["audio_samples"] - 144000) <= 1600, f"lost audio timeline: {stats}"
        print("PASS FFmpeg delayed video retains the shared 3s audio/video timeline")
    print(json.dumps(dict(passed=passed, executable=str(executable))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
