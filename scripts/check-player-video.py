#!/usr/bin/env python3
"""Compute every decoded frame's visible NV12 CRC and check board frame logs.

The software decoder emits its native planar YUV420 format. Only U/V packing
changes: no scaler, CSC, range conversion, rotation or RGB conversion is used.
Board indices are zero based and must occur exactly once, in playback order.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_player_media", ROOT / "scripts/prepare-player-media.py")
MEDIA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MEDIA)
FRAME_MARKER = "VIDEO_FRAME_CRC"
FRAME_LINE = re.compile(r"\bVIDEO_FRAME_CRC\s+index=(\d+)\s+y_crc32=([0-9a-fA-F]{8})\s+uv_crc32=([0-9a-fA-F]{8})\b")


def read_frame(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.read(size - len(data))
        if not part:
            if not data:
                return None
            raise ValueError(f"decoder ended partway through a frame: {len(data)}/{size} bytes")
        data.extend(part)
    return data


def make_reference(media, ffmpeg, output):
    stats = MEDIA.validate_media(media)
    if stats["kind"] != "AVI":
        raise ValueError("video comparison requires a compliant AVI")
    MEDIA.verify_codecs(media, ffmpeg)
    version = subprocess.check_output([str(ffmpeg), "-version"], text=True).splitlines()[0]
    if not re.match(r"^ffmpeg version 9\.0\.2(?:-|\s)", version):
        raise ValueError("reference decoder must be FFmpeg 9.0.2")
    video = next(item for item in MEDIA.probe(media, ffmpeg)["streams"] if item["codec_type"] == "video")
    pixel_format = video["pix_fmt"]
    if pixel_format not in ("yuv420p", "yuvj420p"):
        raise ValueError(f"expected native planar YUV420, got {pixel_format}")
    width, height = video["width"], video["height"]
    y_bytes = width * height
    chroma_bytes = y_bytes // 4
    frame_bytes = y_bytes + 2 * chroma_bytes
    expected = stats["video_frames"]  # Validated against complete AVI/AVIX contents and strh.
    output.parent.mkdir(parents=True, exist_ok=True)
    decode_log = output.with_name("ffmpeg-video-decode.log")
    command = [str(ffmpeg), "-v", "error", "-threads", "1", "-noautorotate", "-i", str(media),
               "-map", "0:v:0", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
               "-c:v", "rawvideo", "-pix_fmt", "+" + pixel_format, "-f", "rawvideo", "pipe:1"]
    frames = []
    with decode_log.open("wb") as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=log)
        try:
            while True:
                planar = read_frame(process.stdout, frame_bytes)
                if planar is None:
                    break
                if len(frames) >= expected:
                    raise ValueError(f"decoder produced extra frame {len(frames)}; AVI declares {expected}")
                # Keep luma unchanged. Interleave the already-decoded U and V
                # samples explicitly, avoiding FFmpeg's automatic format/range conversion.
                uv = bytearray(2 * chroma_bytes)
                uv[0::2] = planar[y_bytes:y_bytes + chroma_bytes]
                uv[1::2] = planar[y_bytes + chroma_bytes:]
                frames.append(dict(index=len(frames),
                                   y_crc32=f"{zlib.crc32(memoryview(planar)[:y_bytes]) & 0xffffffff:08x}",
                                   uv_crc32=f"{zlib.crc32(uv) & 0xffffffff:08x}"))
            status = process.wait()
            if status:
                raise ValueError(f"FFmpeg decode failed ({status}); see {decode_log}")
        except BaseException:
            if process.poll() is None:
                process.kill()
            process.wait()
            raise
        finally:
            process.stdout.close()
    if len(frames) != expected:
        raise ValueError(f"decoder produced {len(frames)} frames; AVI declares {expected}")
    reference = dict(schema=1, media=str(media.resolve()), media_bytes=media.stat().st_size,
                     media_sha256=hashlib.sha256(media.read_bytes()).hexdigest(), ffmpeg=version,
                     native_pixel_format=pixel_format, output_format="NV12", width=width, height=height,
                     visible_stride=width, y_bytes=y_bytes, uv_bytes=2 * chroma_bytes,
                     packing="Y unchanged; U,V interleaved without sample conversion",
                     frame_count=expected, frames=frames)
    output.write_text(json.dumps(reference, indent=2) + "\n", encoding="utf-8")
    return reference


def compare_board(reference, path):
    expected = reference["frames"]
    count = 0
    seen = set()
    with path.open("r", encoding="utf-8-sig", errors="replace") as log:
        for line_number, line in enumerate(log, 1):
            if FRAME_MARKER not in line:
                continue
            matches = list(FRAME_LINE.finditer(line))
            if len(matches) != 1 or line.count(FRAME_MARKER) != 1:
                raise ValueError(f"{path}:{line_number}: malformed or multiple frame records")
            index_text, y_crc, uv_crc = matches[0].groups()
            index = int(index_text)
            if index in seen:
                raise ValueError(f"{path}:{line_number}: duplicate frame {index}")
            if count >= len(expected):
                raise ValueError(f"{path}:{line_number}: extra frame {index}")
            if index != count:
                raise ValueError(f"{path}:{line_number}: expected frame {count}, got {index}")
            record = expected[count]
            if y_crc.lower() != record["y_crc32"] or uv_crc.lower() != record["uv_crc32"]:
                raise ValueError(f"{path}:{line_number}: frame {index} CRC mismatch: "
                                 f"Y {y_crc}/{record['y_crc32']}, UV {uv_crc}/{record['uv_crc32']}")
            seen.add(index)
            count += 1
    if count != len(expected):
        raise ValueError(f"{path}: missing frames: matched {count}/{len(expected)}")
    return dict(board_log=str(path.resolve()), matched_frames=count)


def self_test(reference, output):
    directory = output.parent / "matcher-selftest"
    directory.mkdir(exist_ok=True)
    lines = [f"VIDEO_FRAME_CRC index={frame['index']} y_crc32={frame['y_crc32']} "
             f"uv_crc32={frame['uv_crc32']}\n" for frame in reference["frames"]]
    cases = {"valid": (lines, True)}
    wrong = lines.copy()
    wrong[0] = wrong[0].replace(reference["frames"][0]["y_crc32"],
                              f"{int(reference['frames'][0]['y_crc32'], 16) ^ 1:08x}", 1)
    cases["wrong-crc"] = (wrong, False)
    middle = len(lines) // 2
    cases["missing"] = (lines[:middle] + lines[middle + 1:], False)
    cases["duplicate"] = (lines[:middle] + [lines[middle]] + lines[middle:], False)
    cases["extra"] = (lines + [f"VIDEO_FRAME_CRC index={len(lines)} y_crc32=00000000 uv_crc32=00000000\n"], False)
    cases["missing-tail"] = (lines[:-1], False)
    cases["malformed"] = (["VIDEO_FRAME_CRC index=broken y_crc32=00000000 uv_crc32=00000000\n"], False)
    for name, (content, accepted) in cases.items():
        path = directory / (name + ".log")
        path.write_text("synthetic matcher fixture; no hardware result\n" + "".join(content), encoding="utf-8")
        try:
            compare_board(reference, path)
        except ValueError:
            if accepted:
                raise
        else:
            if not accepted:
                raise AssertionError(f"matcher accepted {name}")
        print(f"PASS synthetic board matcher: {name}")
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, nargs="?", default=ROOT / "build/player-fixture/probe.avi")
    parser.add_argument("--ffmpeg")
    parser.add_argument("--output", type=Path, default=ROOT / "build/player-video-check/video-reference.json")
    parser.add_argument("--board-log", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        reference = make_reference(args.input, MEDIA.find_ffmpeg(args.ffmpeg), args.output)
        result = dict(reference=str(args.output.resolve()), frame_count=reference["frame_count"])
        if args.self_test:
            result["synthetic_cases_passed"] = self_test(reference, args.output)
        if args.board_log:
            result.update(compare_board(reference, args.board_log))
        print(json.dumps(result))
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"video verification failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
