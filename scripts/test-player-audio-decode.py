"""Build real audio decoders, compare FFmpeg PCM, and test rational conversion.

No serial, board, or audio hardware is accessed. Raw bitstream comparisons
include encoder padding; FAAD2's documented first 1024-frame suppression is
accounted explicitly, never inferred by a correlation search.
"""
import argparse
import array
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, **kwargs)
    if result.returncode:
        raise RuntimeError(f"command failed {command}:\n" +
                           (result.stdout + result.stderr).decode("utf-8", errors="replace")[-12000:])
    return result.stdout


def dump_bytes(value):
    data = bytearray()
    for line in value.splitlines():
        if ":" not in line:
            continue
        hex_part = line.split(":", 1)[1].lstrip().split("  ", 1)[0]
        data.extend(bytes.fromhex(hex_part))
    return bytes(data)


def bundle(path, packets):
    with path.open("wb") as output:
        output.write(struct.pack("<I", len(packets)))
        for packet in packets:
            output.write(struct.pack("<I", len(packet)))
            output.write(packet)


def verify_sources():
    for name in ("player-minimp3", "player-faad2"):
        directory = ROOT / "third_party" / name
        source = json.loads((directory / "SOURCE.json").read_text())
        for path, expected in source["files"].items():
            digest = expected["sha256"] if isinstance(expected, dict) else expected
            assert hashlib.sha256((directory / path).read_bytes()).hexdigest() == digest, path


def lame_trim(path):
    """Pinned minimp3_ex.h convention, with a physical Xing/Info frame check."""
    data, offset = path.read_bytes(), 0
    if data[:3] == b"ID3":
        offset = 10 + sum((data[6 + i] & 127) << (21 - 7 * i) for i in range(4))
        if data[5] & 16:
            offset += 10
    header = data[offset:offset + 4]
    assert header[0] == 255 and header[1] & 0xFE == 0xFA  # MPEG1 Layer3.
    mono = header[3] >> 6 == 3
    tag = offset + 4 + (0 if header[1] & 1 else 2) + (17 if mono else 32)
    assert data[tag:tag + 4] in (b"Xing", b"Info")
    flags = int.from_bytes(data[tag + 4:tag + 8], "big")
    assert flags & 1
    cursor = tag + 8
    frames = int.from_bytes(data[cursor:cursor + 4], "big")
    cursor += 4
    for flag, size in ((2, 4), (4, 100), (8, 4)):
        if flags & flag:
            cursor += size
    assert data[cursor:cursor + 4] in (b"LAME", b"Lavc", b"Lavf")
    bits = data[cursor + 21:cursor + 24]
    delay = (bits[0] << 4 | bits[1] >> 4) + 529
    padding = ((bits[1] & 15) << 8 | bits[2]) - 529
    return frames, delay, max(0, padding)


def compare(actual, reference, channels, expected_frames, label):
    a, b = array.array("h"), array.array("h")
    a.frombytes(actual)
    b.frombytes(reference)
    if len(a) != len(b) or len(a) != expected_frames * channels:
        raise AssertionError(f"{label}: length {len(a)} != {len(b)}; expected {expected_frames * channels}")
    error = [int(x) - y for x, y in zip(a, b)]
    rms = math.sqrt(sum(x * x for x in error) / len(error))
    peak = max(map(abs, error))
    signal = math.sqrt(sum(int(x) * x for x in b) / len(b))
    snr = 20 * math.log10(signal / rms) if rms else 200.0
    # A fixed-point decoder differs from FFmpeg's float decoder. Keep a
    # declared fidelity bound on the unchanged sample axis (no fitted gain,
    # offset, or delay): >=45dB SNR, transient peak <=512/32768 full scale.
    if snr < 45.0 or peak > 512:
        raise AssertionError(f"{label}: PCM rms={rms:.5f} peak={peak} snr={snr:.3f}dB; no search/shift applied")
    return {"rms_lsb": rms, "peak_lsb": peak, "snr_db": snr, "compared_frames": expected_frames}


def mips_probe(prefix):
    reports = {}
    for configuration in ("Release", "Debug"):
        directory = ROOT / ("build/player-audio-decode-mips" + ("-debug" if configuration == "Debug" else ""))
        run(["cmake", "-S", ROOT / "projects/halley7m-player/audio-decode", "-B", directory,
             "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + str(ROOT / "cmake/xburst2-elf.cmake"),
             "-DCROSS_COMPILE=" + prefix, "-DCMAKE_BUILD_TYPE=" + configuration])
        run(["cmake", "--build", directory, "--parallel", "8"])
        functions = []
        for path in directory.rglob("*.su"):
            for line in path.read_text().splitlines():
                fields = line.split("\t")
                if len(fields) == 3:
                    functions.append({"function": fields[0], "bytes": int(fields[1]), "mode": fields[2]})
        functions.sort(key=lambda entry: entry["bytes"], reverse=True)
        reports[configuration] = functions
        print(f"PASS real MIPS {configuration} libraries; largest function stack {functions[0]['bytes']}B (not total call-chain stack)", flush=True)
    (ROOT / "build/player-audio-decode-mips-stack.json").write_text(
        json.dumps(reports, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--mips-prefix", help="Also build MIPS Debug/Release and report -fstack-usage; absolute compiler prefix ending in '-'")
    args = parser.parse_args()
    verify_sources()
    output = ROOT / "build/player-audio-decode-host-ninja"
    output.mkdir(parents=True, exist_ok=True)
    environment = None
    if os.name == "nt":
        spec = importlib.util.spec_from_file_location("player_amp_test", ROOT / "scripts/test-player-amp.py")
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        environment = helper.msvc_environment()
    configure = ["cmake", "-S", ROOT / "projects/halley7m-player/audio-decode", "-B", output,
                 "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DPLAYER_AUDIO_DECODE_HOST_TEST=ON"]
    if os.name == "nt":
        # Embedded debug data avoids a host MSPDB server dependency during
        # CMake's default-Debug compiler probe. The real test build is Release.
        configure.append("-DCMAKE_C_FLAGS_DEBUG=/D_DEBUG /MTd /Ob0 /Od /RTC1 /Z7")
    run(configure, env=environment)
    run(["cmake", "--build", output, "--parallel", "8"], env=environment)
    suffix = ".exe" if os.name == "nt" else ""
    print(run([output / ("player_pcm_convert_test" + suffix)]).decode().strip(), flush=True)
    decode = output / ("player_audio_decode_test" + suffix)
    ffmpeg = args.ffmpeg
    if not ffmpeg:
        deps = json.loads((ROOT / "build/player-tools/dependencies.json").read_text())
        ffmpeg = Path(deps["ffmpeg"]["executable"])
    ffprobe = ffmpeg.with_name("ffprobe" + ffmpeg.suffix)
    fixtures = output / "fixtures"
    fixtures.mkdir(exist_ok=True)
    report = []
    for rate in (44100, 48000):
        for channels in (1, 2):
            name = f"{rate}-{channels}ch"
            pcm = array.array("h")
            count = rate // 3 + 17
            for i in range(count):
                for channel in range(channels):
                    value = 6000 * math.sin(2 * math.pi * (317 + channel * 137) * i / rate)
                    value += 1400 * math.sin(2 * math.pi * 1703 * i / rate)
                    pcm.append(round(value))
            source = fixtures / (name + ".s16")
            source.write_bytes(pcm.tobytes())
            mp3, adts = fixtures / (name + ".mp3"), fixtures / (name + ".aac")
            common = [ffmpeg, "-v", "error", "-y", "-f", "s16le", "-ar", rate,
                      "-ac", channels, "-i", source]
            run(common + ["-c:a", "libmp3lame", "-b:a", "128k", "-write_xing", "0",
                          "-id3v2_version", "0", "-write_id3v1", "0", mp3])
            run(common + ["-c:a", "aac", "-profile:a", "aac_low", "-b:a", "128k", "-f", "adts", adts])
            m4a = fixtures / (name + ".m4a")
            run([ffmpeg, "-v", "error", "-y", "-i", adts, "-c:a", "copy", m4a])
            gapless = fixtures / (name + ".gapless.mp3")
            run(common + ["-c:a", "libmp3lame", "-b:a", "128k", gapless])
            for codec, media, tagged in ((1, mp3, False), (2, adts, False),
                                          (3, m4a, False), (1, gapless, True)):
                label = f"codec{codec}-{name}" + ("-xing-lame" if tagged else "")
                meta = json.loads(run([ffprobe, "-v", "error", "-select_streams", "a:0",
                                       "-show_packets", "-show_streams", "-show_data", "-of", "json", media]))
                packets = [dump_bytes(packet["data"]) for packet in meta["packets"]]
                path = fixtures / (label + ".packets")
                bundle(path, packets)
                native = fixtures / (label + ".native.s16")
                stats_path = fixtures / (label + ".stats.json")
                command = [decode, codec, path, native, stats_path]
                if codec == 3:
                    asc = fixtures / (label + ".asc")
                    asc.write_bytes(dump_bytes(meta["streams"][0]["extradata"]))
                    command.append(asc)
                run(command)
                stats = json.loads(stats_path.read_text())
                reference = run([ffmpeg, "-v", "error", "-i", media, "-f", "s16le", "-acodec", "pcm_s16le", "-"])
                skipped = 0 if codec == 1 else 1024
                assert stats["decoder_skipped_frames"] == skipped
                assert stats["sample_rate"] == rate and stats["channels"] == channels
                actual = native.read_bytes()
                trimmed, leading, trailing = stats["frames"], 0, 0
                if tagged:
                    tag_frames, leading, trailing = lame_trim(media)
                    # ffprobe excludes metadata frame; core sees only audio.
                    assert len(packets) == tag_frames and stats["frames"] == tag_frames * 1152
                    assert stats["frames"] - leading - trailing == count
                    actual = actual[leading * channels * 2:len(actual) - trailing * channels * 2]
                    trimmed -= leading + trailing
                assert len(reference) // (channels * 2) - skipped == trimmed
                error = compare(actual, reference[skipped * channels * 2:], channels, trimmed, label)
                report.append({"label": label, **stats, **error, "reference_skip_frames": skipped,
                               "trim_leading": leading, "trim_trailing": trailing})
                print(f"PASS {label}: {stats['frames']} source frames, skipped={skipped}, rms={error['rms_lsb']:.3f} peak={error['peak_lsb']}", flush=True)
                bad = fixtures / (label + ".truncated")
                bundle(bad, [packets[0][:1]])
                invalid_command = [decode, codec, bad, native.with_suffix(".invalid"), stats_path.with_suffix(".invalid")]
                if codec == 3:
                    invalid_command.append(asc)
                rejected = subprocess.run(list(map(str, invalid_command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                assert rejected.returncode != 0, label + " accepted truncated packet"
    (output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS 16 real FFmpeg MP3/ADTS/MP4-raw AAC-LC fixtures including Xing/LAME exact gapless trim; no board/audio output tested", flush=True)
    if args.mips_prefix:
        mips_probe(args.mips_prefix)


if __name__ == "__main__":
    main()
