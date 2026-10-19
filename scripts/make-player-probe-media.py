#!/usr/bin/env python3
"""Create the bounded, silent player hardware fixture and first-frame CRCs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_player_media", ROOT / "scripts/prepare-player-media.py")
MEDIA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MEDIA)

DIGITS = (
    ("01110", "11011", "11011", "11011", "11011", "11011", "01110"),
    ("00110", "01110", "00110", "00110", "00110", "00110", "01111"),
    ("01110", "11011", "00011", "00110", "01100", "11000", "11111"),
    ("11110", "00011", "00011", "01110", "00011", "00011", "11110"),
    ("00011", "00111", "01111", "11011", "11111", "00011", "00011"),
    ("11111", "11000", "11000", "11110", "00011", "00011", "11110"),
    ("01110", "11000", "11000", "11110", "11011", "11011", "01110"),
    ("11111", "00011", "00110", "00110", "01100", "01100", "01100"),
    ("01110", "11011", "11011", "01110", "11011", "11011", "01110"),
    ("01110", "11011", "11011", "01111", "00011", "00011", "01110"),
)


def rectangle(pixels, x, y, width, height, color):
    row = bytes(color) * width
    for line in range(y, y + height):
        offset = (line * 1280 + x) * 3
        pixels[offset:offset + len(row)] = row


def picture(index):
    pixels = bytearray(1280 * 720 * 3)
    colors = ((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
              (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0))
    for column, color in enumerate(colors):
        rectangle(pixels, column * 160, 0, 160, 480, color)
    for level in range(16):
        rectangle(pixels, level * 80, 480, 80, 240, (level * 17,) * 3)
    # Four deliberately different corner marks expose orientation/crop errors.
    rectangle(pixels, 16, 16, 96, 32, (255, 0, 255))
    rectangle(pixels, 16, 16, 32, 96, (255, 0, 255))
    rectangle(pixels, 1168, 16, 96, 64, (0, 255, 255))
    rectangle(pixels, 16, 640, 64, 64, (255, 255, 0))
    rectangle(pixels, 1168, 608, 96, 96, (255, 255, 255))
    rectangle(pixels, 1168, 608, 48, 48, (0, 0, 0))
    rectangle(pixels, 544, 576, 192, 80, (0, 0, 0))
    for digit_index, character in enumerate(f"{index:03d}"):
        for row_index, row in enumerate(DIGITS[int(character)]):
            for column, bit in enumerate(row):
                if bit == "1":
                    rectangle(pixels, 560 + digit_index * 56 + column * 8,
                              588 + row_index * 8, 8, 8, (255, 255, 255))
    rectangle(pixels, 96 + (index % 32) * 32, 680, 16, 24, (255, 255, 255))
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg")
    parser.add_argument("--frames", type=int, choices=range(12, 301), default=90)
    args = parser.parse_args()
    ffmpeg = MEDIA.find_ffmpeg(args.ffmpeg)
    output = ROOT / "build/player-fixture"
    output.mkdir(parents=True, exist_ok=True)
    source = output / "source.mkv"
    prepared = output / "probe.avi"
    # RGB source avoids a font dependency and preserves the independent pattern
    # before the production converter applies rotation, matrix and full range.
    command = [str(ffmpeg), "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
               "-video_size", "1280x720", "-framerate", "30", "-i", "pipe:0",
               "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono", "-map", "0:v:0", "-map", "1:a:0",
               "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", "-shortest", str(source)]
    with (output / "source-ffmpeg.log").open("wb") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log)
        try:
            for index in range(args.frames):
                process.stdin.write(picture(index))
        finally:
            process.stdin.close()
        if process.wait():
            raise RuntimeError(f"source FFmpeg failed; see {output / 'source-ffmpeg.log'}")
    MEDIA.convert(source, prepared, ffmpeg, True)
    stats = MEDIA.validate_media(prepared)
    MEDIA.verify_codecs(prepared, ffmpeg)
    if stats["video_frames"] != args.frames:
        raise RuntimeError(f"fixture frame count changed: {stats}")
    nv12_path = output / "frame0.nv12"
    subprocess.run([str(ffmpeg), "-v", "error", "-y", "-i", str(prepared), "-map", "0:v:0",
                    "-frames:v", "1", "-vf", "scale=in_range=full:out_range=full,format=nv12",
                    "-pix_fmt", "nv12", "-color_range", "pc", "-f", "rawvideo", str(nv12_path)], check=True)
    nv12 = nv12_path.read_bytes()
    y_bytes, uv_bytes = 720 * 1280, 720 * 640
    if len(nv12) != y_bytes + uv_bytes:
        raise RuntimeError("unexpected software NV12 plane sizes")
    # Confirm the reference packet's audio is actually silent, rather than
    # merely relying on a zero-volume playback setting or a missing track.
    pcm = subprocess.run([str(ffmpeg), "-v", "error", "-i", str(prepared), "-map", "0:a:0",
                          "-f", "s16le", "-c:a", "pcm_s16le", "pipe:1"],
                         check=True, capture_output=True).stdout
    if not pcm or any(pcm):
        raise RuntimeError("fixture PCM must be present and exactly zero")
    report = dict(file=str(prepared.resolve()), bytes=prepared.stat().st_size,
                  sha256=hashlib.sha256(prepared.read_bytes()).hexdigest(), **stats,
                  audio_silent=True,
                  reference=dict(file=str(nv12_path.resolve()), format="NV12", width=720, height=1280,
                                 visible_stride=720, board_stride=768, matrix="BT.601", color_range="full",
                                 frame_index=0, y_bytes=y_bytes, uv_bytes=uv_bytes,
                                 y_crc32=f"{zlib.crc32(nv12[:y_bytes]) & 0xffffffff:08x}",
                                 uv_crc32=f"{zlib.crc32(nv12[y_bytes:]) & 0xffffffff:08x}",
                                 nv12_crc32=f"{zlib.crc32(nv12) & 0xffffffff:08x}"))
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
