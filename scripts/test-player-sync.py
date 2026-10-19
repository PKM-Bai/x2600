#!/usr/bin/env python3
"""Generate local MP4 captures, then exercise the production sync CLI end to end."""
import argparse
from array import array
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[1]
FFMPEG = ROOT / "tools/ffmpeg/bin" / ("ffmpeg.exe" if sys.platform == "win32" else "ffmpeg")
ANALYZER = ROOT / "scripts/analyze-player-sync.py"


def fixture(output, name, offset, *, fps=30, rate=48000, vfr=False, missing=False, noise_only=False, extra_tone=False):
    seconds, audio_start = 4, 0.17
    flashes = [0.5, 1.5, 2.5]
    tones = [] if noise_only else [value + offset for value in flashes]
    if extra_tone:
        tones.append(3.15)
    randomizer = random.Random(2600)
    samples = array("h")
    # Constant broadband noise plus an unrelated low-frequency tone prevents
    # the detector from succeeding by treating all non-silence as a test pulse.
    for sample in range(round((seconds - audio_start) * rate)):
        time = audio_start + sample / rate
        value = randomizer.gauss(0, 0.004 if noise_only else 0.0015)
        if 3.25 <= time < 3.31:
            value += 0.12 * math.cos(2 * math.pi * 500 * time)
        for onset in tones:
            if onset <= time < onset + 1 / 30:
                value += 0.3 * math.cos(2 * math.pi * 1500 * (time - onset))
        samples.append(round(max(-1, min(1, value)) * 32767))
    if sys.byteorder != "little":
        samples.byteswap()
    wav = output / (name + ".wav")
    with wave.open(str(wav), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        writer.writeframes(samples.tobytes())
    indices = [round(value * fps) for value in ([0.5, 2.5] if missing else flashes)]
    enabled = "+".join(f"eq(n,{index})" for index in indices)
    source = (f"color=c=0x101010:s=160x96:r={fps}:d=4,"
        f"drawbox=x=4:y=4:w=64:h=40:color=white:t=fill:enable='{enabled}',"
        # Bright motion outside the screen ROI must not count as a flash.
        "drawbox=x=100:y=48:w=40:h=40:color=white:t=fill:enable='lt(mod(n,11),3)'")
    command = [str(FFMPEG), "-v", "error", "-nostdin", "-y", "-copyts", "-f", "lavfi", "-i", source,
        "-itsoffset", str(audio_start), "-i", str(wav), "-map", "0:v:0", "-map", "1:a:0"]
    if vfr:
        command += ["-vf", "select='not(eq(mod(n,7),2))'", "-fps_mode", "vfr"]
    command += ["-c:v", "libx264", "-preset", "ultrafast", "-crf", "18", "-pix_fmt", "yuv420p",
        "-bf", "0", "-g", "30", "-c:a", "aac", "-b:a", "192k", "-t", "4", str(output / (name + ".mp4"))]
    subprocess.run(command, check=True)
    return output / (name + ".mp4")


def analyze(output, name, capture, *, accepted=True, extra=()):
    report = output / (name + ".json")
    result = subprocess.run([sys.executable, str(ANALYZER), str(capture), "--roi", "8,8,48,28",
        "--output", str(report), "--synthetic", "--expected-pairs", "3", *extra], capture_output=True, text=True)
    data = json.loads(report.read_text(encoding="utf-8"))
    assert data["kind"] == "synthetic", data
    if accepted:
        assert result.returncode == 0 and data["status"] == "analyzed", result.stderr + json.dumps(data)
        assert len(data["pairs"]) == 3 and data["summary"]["acceptance"].startswith("not evaluated")
    else:
        assert result.returncode != 0 and data["status"] == "error" and data["error"], result.stdout
    print("PASS synthetic sync:", name, "(not physical acceptance)", flush=True)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/player-sync-tests")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    plus = fixture(output, "plus80-nonzero-aac", 0.080)
    report = analyze(output, "plus80", plus)
    assert report["audio"]["first_sample_pts_s"] > 0.1  # Must add this PTS, not zero each stream.
    assert abs(report["summary"]["raw_median_ms"] - 80) < 5, report["summary"]
    assert all(abs(pair["raw_audio_minus_optical_ms"] - 80) < 5 for pair in report["pairs"])
    assert report["summary"]["corrected_median_ms"] is None
    shifted = output / "whole-capture-shift.mp4"
    subprocess.run([str(FFMPEG), "-v", "error", "-nostdin", "-y", "-copyts", "-i", str(plus),
        "-map", "0", "-c", "copy", "-output_ts_offset", "2", str(shifted)], check=True)
    shifted_report = analyze(output, "whole-capture-shift", shifted)
    assert shifted_report["video"]["first_pts_s"] >= 2
    assert shifted_report["audio"]["first_sample_pts_s"] >= 2
    assert abs(shifted_report["summary"]["raw_median_ms"] - report["summary"]["raw_median_ms"]) < 0.01
    calibrated = analyze(output, "capture-bias", plus,
        extra=("--calibration-video-contact", "0.2", "--calibration-audio-contact", "0.33",
               "--calibration-uncertainty-ms", "8.4"))
    assert abs(calibrated["summary"]["corrected_median_ms"] + 50) < 5
    assert abs(calibrated["calibration"]["capture_audio_minus_video_bias_ms"] - 130) < 0.001
    for pair in calibrated["pairs"]:
        narrow, expanded = pair["corrected_sampling_bounds_ms"], pair["corrected_bounds_with_manual_uncertainty_ms"]
        assert abs(narrow[0] - expanded[0] - 8.4) < 0.001 and abs(expanded[1] - narrow[1] - 8.4) < 0.001
    vfr = fixture(output, "minus50-vfr", -0.050, vfr=True)
    report = analyze(output, "minus50-vfr", vfr)
    assert abs(report["summary"]["raw_median_ms"] + 50) < 5, report["summary"]
    assert report["video"]["frame_interval_max_ms"] > 1.9 * report["video"]["frame_interval_median_ms"]
    assert abs(report["summary"]["ideal_frame_resolution_ms"]["60fps_one_frame"] - 1000 / 60) < 0.001
    sixty = fixture(output, "sixty-fps", 0.080, fps=60)
    report = analyze(output, "sixty-fps", sixty)
    assert abs(report["summary"]["raw_median_ms"] - 80) < 5
    assert abs(report["video"]["frame_interval_median_ms"] - 1000 / 60) < 0.01
    native_rate = fixture(output, "native-44100", 0.080, rate=44100)
    report = analyze(output, "native-44100", native_rate)
    assert report["audio"]["sample_rate"] == 44100
    assert abs(report["summary"]["raw_median_ms"] - 80) < 5
    missing = fixture(output, "missing-flash", 0.080, missing=True)
    analyze(output, "missing-flash", missing, accepted=False)
    noise = fixture(output, "noise-only", 0, noise_only=True)
    analyze(output, "noise-only", noise, accepted=False)
    extra = fixture(output, "extra-tone", 0.080, extra_tone=True)
    analyze(output, "extra-tone", extra, accepted=False)
    analyze(output, "expected-count", plus, accepted=False, extra=("--expected-pairs", "4"))
    analyze(output, "wrong-roi", plus, accepted=False, extra=("--roi", "100,50,40,30"))
    print(f"PASS synthetic end-to-end sync suite; local captures/reports: {output}")


if __name__ == "__main__":
    main()
