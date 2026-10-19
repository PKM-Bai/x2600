#!/usr/bin/env python3
"""Convert and validate the fixed AVI/WAV profile used by halley7m-player."""
from __future__ import annotations

import argparse
from fractions import Fraction
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
VIDEO_MAX = 512 * 1024
FAT32_LIMIT = 1 << 32


def u32(data: bytes, offset: int = 0) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def read_exact(handle, offset: int, length: int) -> bytes:
    handle.seek(offset)
    data = handle.read(length)
    if len(data) != length:
        raise ValueError(f"truncated data at {offset}")
    return data


def chunks(handle, start: int, end: int):
    while start < end:
        if end - start < 8:
            raise ValueError(f"incomplete RIFF chunk header at {start}")
        header = read_exact(handle, start, 8)
        tag, length = header[:4], u32(header, 4)
        after = start + 8 + length + (length & 1)
        if after > end:
            raise ValueError(f"RIFF chunk {tag!r} extends past its parent")
        yield tag, start + 8, length
        start = after


def validate_au(data: bytes, first: bool) -> None:
    starts = list(re.finditer(b"\x00\x00(?:\x00)?\x01", data))
    if not starts or starts[0].start() != 0:
        raise ValueError("video chunk is not Annex B")
    types = []
    for match in starts:
        if match.end() >= len(data) or data[match.end()] & 0x80:
            raise ValueError("invalid H264 NAL header")
        types.append(data[match.end()] & 31)
    if types.count(9) != 1 or not any(kind in (1, 5) for kind in types):
        raise ValueError("each video chunk must hold one AUD and one access unit")
    if first and (7 not in types or 8 not in types or 5 not in types or
                  types.index(7) > types.index(5) or types.index(8) > types.index(5)):
        raise ValueError("first video chunk needs SPS/PPS before IDR")


def pcm_format(data: bytes) -> None:
    if len(data) < 16 or struct.unpack_from("<HHIIHH", data) != (1, 1, 48000, 96000, 2, 16):
        raise ValueError("audio must be PCM S16LE, 48000 Hz, mono")


def stream_headers(handle, start: int, end: int) -> list[dict]:
    streams = []
    for tag, offset, length in chunks(handle, start, end):
        if tag != b"LIST" or length < 4 or read_exact(handle, offset, 4) != b"strl":
            continue
        stream = {}
        for child, child_offset, child_length in chunks(handle, offset + 4, offset + length):
            if child == b"strh":
                data = read_exact(handle, child_offset, min(child_length, 56))
                if len(data) < 56:
                    raise ValueError("short strh")
                stream.update(type=data[:4], scale=u32(data, 20), rate=u32(data, 24),
                              start=u32(data, 28), count=u32(data, 32))
            elif child == b"strf":
                stream["format"] = read_exact(handle, child_offset, min(child_length, 40))
        streams.append(stream)
    return streams


def validate_media(path: Path) -> dict:
    """Scan actual file chunks without loading the file or indexes into RAM."""
    size = path.stat().st_size
    if size >= FAT32_LIMIT:
        raise ValueError("FAT32 requires a file smaller than 4 GiB")
    with path.open("rb") as handle:
        header = read_exact(handle, 0, 12)
        if header[:4] != b"RIFF" or u32(header, 4) < 4:
            raise ValueError("expected RIFF AVI/WAVE")
        first_end = 8 + u32(header, 4)
        if first_end > size:
            raise ValueError("RIFF extends past EOF")
        if header[8:] == b"WAVE":
            have_format = False
            for tag, offset, length in chunks(handle, 12, first_end):
                if tag == b"fmt ":
                    pcm_format(read_exact(handle, offset, min(length, 16)))
                    have_format = True
                elif tag == b"data":
                    if not have_format or length & 1:
                        raise ValueError("invalid PCM data")
                    return dict(kind="WAV", audio_samples=length // 2, duration=length / 96000)
            raise ValueError("missing WAV data")
        if header[8:] != b"AVI ":
            raise ValueError("expected AVI or WAVE")
        streams = []
        for tag, offset, length in chunks(handle, 12, first_end):
            if tag == b"LIST" and length >= 4 and read_exact(handle, offset, 4) == b"hdrl":
                streams = stream_headers(handle, offset + 4, offset + length)
                break
        if len(streams) != 2:
            raise ValueError("AVI needs exactly one video and one audio stream")
        video = audio = None
        for index, stream in enumerate(streams):
            if not all(key in stream for key in ("type", "format", "scale", "rate", "count", "start")):
                raise ValueError("incomplete AVI stream header")
            if stream["start"] or not stream["scale"]:
                raise ValueError("stream must begin at zero")
            if stream["type"] == b"vids":
                data = stream["format"]
                if (video is not None or len(data) < 40 or u32(data) < 40 or
                    struct.unpack_from("<ii", data, 4) != (720, 1280) or data[16:20] != b"H264" or
                    stream["rate"] != stream["scale"] * 30):
                    raise ValueError("video must be H264 720x1280 CFR30")
                video = index
            elif stream["type"] == b"auds":
                if audio is not None or stream["rate"] != stream["scale"] * 48000:
                    raise ValueError("invalid audio stream clock")
                pcm_format(stream["format"][:16])
                audio = index
        if video is None or audio is None:
            raise ValueError("missing video/audio stream")
        video_tag, audio_tag = f"{video:02d}dc".encode(), f"{audio:02d}wb".encode()
        frames = samples = riffs = 0
        max_packet = 0

        def scan_movi(start: int, end: int, depth: int = 0):
            nonlocal frames, samples, max_packet
            if depth >= 7:
                raise ValueError("RIFF nesting too deep")
            for tag, offset, length in chunks(handle, start, end):
                if tag == b"LIST" and length >= 4 and read_exact(handle, offset, 4) == b"rec ":
                    scan_movi(offset + 4, offset + length, depth + 1)
                elif tag == video_tag:
                    if not 0 < length <= VIDEO_MAX:
                        raise ValueError("video packet exceeds 512 KiB or is empty")
                    validate_au(read_exact(handle, offset, length), frames == 0)
                    frames += 1
                    max_packet = max(max_packet, length)
                elif tag == audio_tag:
                    if length & 1:
                        raise ValueError("PCM chunk is not sample aligned")
                    samples += length // 2

        position = 0
        while position < size:
            root = read_exact(handle, position, 12)
            if root[:4] != b"RIFF" or root[8:] != (b"AVI " if riffs == 0 else b"AVIX"):
                raise ValueError("expected AVI/AVIX RIFF")
            end = position + 8 + u32(root, 4)
            if end > size or end < position + 12:
                raise ValueError("invalid RIFF size")
            for tag, offset, length in chunks(handle, position + 12, end):
                if tag == b"LIST" and length >= 4 and read_exact(handle, offset, 4) == b"movi":
                    scan_movi(offset + 4, offset + length)
            riffs += 1
            position = end + (end & 1)
        if not frames or frames != streams[video]["count"] or samples != streams[audio]["count"]:
            raise ValueError("stream length disagrees with complete AVI/AVIX contents")
        if abs(samples / 48000 - frames / 30) > 0.1:
            raise ValueError("audio/video tail differs by more than 100 ms")
        return dict(kind="AVI", video_frames=frames, audio_samples=samples, riff_count=riffs,
                    max_video_packet=max_packet, duration=frames / 30)


def find_ffmpeg(explicit: str | None) -> Path:
    if explicit:
        result = Path(explicit).resolve()
    else:
        bundled = ROOT / "tools/ffmpeg/bin/ffmpeg.exe"
        result = bundled if bundled.exists() else Path(shutil.which("ffmpeg") or "ffmpeg")
    if not result.is_file():
        raise ValueError("FFmpeg not found; use --ffmpeg PATH")
    return result


def probe(path: Path, ffmpeg: Path) -> dict:
    ffprobe = ffmpeg.with_name("ffprobe" + ffmpeg.suffix)
    result = subprocess.run([str(ffprobe), "-v", "error", "-show_streams", "-show_format",
                             "-of", "json", str(path)], check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def video_duration(path: Path, ffmpeg: Path, information: dict) -> Fraction:
    """Find the video end on FFmpeg's shared, input-start-normalized timeline.

    Packet timestamps avoid trusting a container duration that may instead
    describe a longer audio track. This scan neither decodes nor buffers video.
    """
    video = next(item for item in information["streams"] if item["codec_type"] == "video")
    rate_text = video.get("avg_frame_rate", "0/1")
    rate = Fraction(0) if rate_text == "0/0" else Fraction(rate_text)
    fallback = 1 / rate if rate > 0 else Fraction(1, 30)
    origin = Fraction(information.get("format", {}).get("start_time", "0"))
    ffprobe = ffmpeg.with_name("ffprobe" + ffmpeg.suffix)
    command = [str(ffprobe), "-v", "error", "-select_streams", "v:0", "-show_packets",
               "-show_entries", "packet=pts_time,duration_time", "-of", "compact=p=0:nk=0", str(path)]
    end = None
    with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as process:
        for line in process.stdout:
            fields = dict(field.split("=", 1) for field in line.strip().split("|") if "=" in field)
            if fields.get("pts_time", "N/A") == "N/A":
                continue
            duration = fields.get("duration_time", "N/A")
            duration = Fraction(duration) if duration != "N/A" else fallback
            timestamp = Fraction(fields["pts_time"]) + (duration if duration > 0 else fallback)
            end = timestamp if end is None else max(end, timestamp)
        if process.wait():
            raise subprocess.CalledProcessError(process.returncode, command)
    if end is None or end <= origin:
        raise ValueError("input video has no usable presentation timestamps")
    # fps uses nearest rounding; make the PCM endpoint the same whole frame.
    frames = int((end - origin) * 30 + Fraction(1, 2))
    if not frames:
        raise ValueError("input video is shorter than one output frame")
    return Fraction(frames, 30)


def verify_codecs(path: Path, ffmpeg: Path) -> None:
    streams = probe(path, ffmpeg)["streams"]
    for stream in streams:
        if stream["codec_type"] == "video":
            if (stream["codec_name"] != "h264" or stream.get("profile") not in ("Baseline", "Constrained Baseline") or
                stream.get("level") != 31 or stream.get("has_b_frames") != 0 or
                stream.get("pix_fmt") not in ("yuv420p", "yuvj420p") or
                stream.get("refs", 1) != 1 or stream.get("r_frame_rate") != "30/1" or
                stream.get("width") != 720 or stream.get("height") != 1280 or
                stream.get("color_range") != "pc" or stream.get("color_space") != "smpte170m"):
                raise ValueError("FFprobe rejected the H264 profile/color contract")
        elif (stream["codec_type"] != "audio" or stream["codec_name"] != "pcm_s16le" or
              stream.get("sample_rate") != "48000" or stream.get("channels") != 1):
            raise ValueError("FFprobe rejected the PCM contract")


def convert(source: Path, destination: Path, ffmpeg: Path, overwrite: bool) -> None:
    information = probe(source, ffmpeg)
    streams = information["streams"]
    has_video = any(item["codec_type"] == "video" for item in streams)
    has_audio = any(item["codec_type"] == "audio" for item in streams)
    command = [str(ffmpeg), "-hide_banner", "-y" if overwrite else "-n", "-i", str(source)]
    if destination.suffix.lower() == ".wav":
        if not has_audio:
            raise ValueError("input contains no audio")
        command += ["-map", "0:a:0", "-vn", "-c:a", "pcm_s16le", "-ar", "48000", "-ac", "1"]
    elif destination.suffix.lower() == ".avi":
        if not has_video:
            raise ValueError("AVI conversion requires an input video")
        duration = video_duration(source, ffmpeg, information)
        if not has_audio:
            command += ["-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono"]
        video_filter = ("scale=1280:720:force_original_aspect_ratio=decrease:force_divisible_by=2:reset_sar=1:"
                        "out_color_matrix=bt601:out_range=full,"
                        "pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=black,"
                        "transpose=clock,setsar=1,fps=fps=30:start_time=0,format=yuv420p")
        command += ["-map", "0:v:0", "-map", "0:a:0" if has_audio else "1:a:0", "-vf", video_filter,
                    "-c:v", "libx264", "-preset", "medium", "-profile:v", "baseline", "-level:v", "3.1",
                    "-pix_fmt", "yuv420p", "-fps_mode", "cfr", "-r", "30", "-flags:v", "-global_header",
                    "-g", "30", "-bf", "0", "-refs", "1", "-coder", "0", "-sc_threshold", "0",
                    "-x264-params", "annexb=1:repeat-headers=1:aud=1:open-gop=0:force-cfr=1",
                    "-b:v", "2M", "-maxrate", "3M", "-bufsize", "3M", "-vtag", "H264",
                    "-colorspace", "smpte170m", "-color_range", "pc", "-color_primaries", "smpte170m",
                    "-color_trc", "smpte170m", "-af", "aresample=48000:async=1:first_pts=0,apad",
                    "-c:a", "pcm_s16le", "-ar", "48000", "-ac", "1", "-shortest",
                    "-t", f"{float(duration):.9f}"]
    else:
        raise ValueError("output extension must be .avi or .wav")
    if source.resolve() == destination.resolve():
        raise ValueError("input and output paths must differ")
    command += ["-map_metadata", "-1", str(destination)]
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--ffmpeg")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()
    try:
        ffmpeg = find_ffmpeg(args.ffmpeg)
        if not args.validate_only:
            if args.output is None:
                parser.error("output is required unless --validate-only is given")
            convert(args.input, args.output, ffmpeg, args.overwrite)
        target = args.input if args.validate_only else args.output
        result = validate_media(target)
        verify_codecs(target, ffmpeg)
        print(json.dumps(dict(file=str(target.resolve()), **result), ensure_ascii=False))
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"media preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
