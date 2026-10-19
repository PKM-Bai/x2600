#!/usr/bin/env python3
"""Measure recorded screen flashes against 1500 Hz tones on one capture PTS axis.

ROI coordinates refer to encoded pixels: autorotation is deliberately disabled.
No player-internal clock is treated as a physical sound/light measurement.
"""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


class AnalysisError(ValueError):
    pass


def run_json(command):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return json.loads(result.stdout)


def executable(name, explicit):
    candidate = Path(explicit) if explicit else ROOT / "tools/ffmpeg/bin" / (name + (".exe" if sys.platform == "win32" else ""))
    if candidate.is_file():
        return str(candidate.resolve())
    found = shutil.which(explicit or name)
    if not found:
        raise AnalysisError(f"{name} not found; provide --{name}")
    return found


def timestamp(frame):
    value = frame.get("best_effort_timestamp_time", frame.get("pts_time"))
    if value is None or not math.isfinite(float(value)):
        raise AnalysisError("decoded frame has no finite presentation timestamp")
    return float(value)


def frame_probe(ffprobe, path, stream):
    return run_json([ffprobe, "-v", "error", "-select_streams", stream, "-show_frames",
        "-show_entries", "frame=pts_time,best_effort_timestamp_time,duration_time,nb_samples",
        "-of", "json", str(path)]).get("frames", [])


def decode(ffmpeg, arguments, output):
    result = subprocess.run([ffmpeg, "-v", "error", "-nostdin", "-y", *arguments, str(output)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if result.returncode:
        raise AnalysisError("FFmpeg decode failed: " + result.stderr.decode("utf-8", errors="replace")[-2000:])


def optical_events(frames, levels, threshold):
    if len(frames) != len(levels) or len(frames) < 3:
        raise AnalysisError(f"video timestamp/pixel count mismatch: {len(frames)}/{len(levels)}")
    pts = [timestamp(frame) for frame in frames]
    intervals = [right - left for left, right in zip(pts, pts[1:])]
    if any(value <= 0 for value in intervals):
        raise AnalysisError("video presentation timestamps are not strictly increasing")
    baseline, peak = statistics.median(levels), max(levels)
    if threshold is None:
        if peak - baseline < 30:
            raise AnalysisError("ROI has insufficient flash contrast; choose a smaller screen ROI or explicit threshold")
        threshold = baseline + (peak - baseline) * 0.7
    low = baseline + (threshold - baseline) * 0.6
    events, inside = [], False
    for index, value in enumerate(levels):
        if not inside and value >= threshold:
            if not index:
                raise AnalysisError("flash touches capture start; retain a dark frame before the first pulse")
            events.append(dict(frame_index=index, first_bright_pts_s=pts[index],
                previous_dark_pts_s=pts[index - 1], frame_interval_ms=(pts[index] - pts[index - 1]) * 1000,
                roi_mean=value))
            inside = True
        elif inside and value < low:
            events[-1]["first_dark_after_pts_s"] = pts[index]
            inside = False
    if inside:
        raise AnalysisError("flash touches capture end; retain a dark frame after the final pulse")
    return events, dict(frame_count=len(frames), first_pts_s=pts[0], last_pts_s=pts[-1],
        frame_interval_median_ms=statistics.median(intervals) * 1000,
        frame_interval_min_ms=min(intervals) * 1000, frame_interval_max_ms=max(intervals) * 1000,
        roi_baseline=baseline, roi_peak=peak, flash_threshold=threshold)


def audio_events(frames, raw_path, rate, frequency, minimum_ms):
    """Sine/cosine projection avoids causal band-pass group delay.

    Decode to native-rate mono float, map each decoded frame to its original
    PTS, and never synthesize silence or collapse a timestamp gap. Small windows
    can cross contiguous codec frames but cannot cross a discontinuity.
    """
    if not frames or rate < frequency * 2.5:
        raise AnalysisError("no decoded audio or sample rate too low for the requested tone")
    size, hop = round(rate * 0.004), round(rate * 0.002)
    cosine = [math.cos(2 * math.pi * frequency * i / rate) for i in range(size)]
    sine = [math.sin(2 * math.pi * frequency * i / rate) for i in range(size)]
    times, bands, ratios = array("d"), array("f"), array("f")
    segments = array("I")
    buffer = array("f")
    buffer_pts, prior_end, segment = 0.0, None, 0
    gaps, samples = [], 0
    first_pts = timestamp(frames[0])
    with raw_path.open("rb") as source:
        for frame in frames:
            pts, count = timestamp(frame), int(frame.get("nb_samples", 0))
            if count <= 0:
                raise AnalysisError("audio frame lacks a positive decoded sample count")
            payload = source.read(count * 4)
            if len(payload) != count * 4:
                raise AnalysisError("audio PCM ended before ffprobe's decoded sample count")
            block = array("f")
            block.frombytes(payload)
            if sys.byteorder != "little":
                block.byteswap()
            if prior_end is None or abs(pts - prior_end) > 2.0 / rate:
                if prior_end is not None:
                    if pts < prior_end:
                        raise AnalysisError("audio presentation timestamps overlap or go backwards")
                    gaps.append(dict(start_s=prior_end, end_s=pts))
                buffer = array("f")
                buffer_pts = pts
                segment += 1
            buffer.extend(block)
            consumed = 0
            while len(buffer) - consumed >= size:
                real = imaginary = energy = 0.0
                for i in range(size):
                    value = buffer[consumed + i]
                    real += value * cosine[i]
                    imaginary += value * sine[i]
                    energy += value * value
                rms = math.sqrt(energy / size)
                band = math.sqrt(2 * (real * real + imaginary * imaginary)) / size
                times.append(buffer_pts + consumed / rate)
                bands.append(band)
                ratios.append(min(1.0, band / rms) if rms else 0)
                segments.append(segment)
                consumed += hop
            if consumed:
                del buffer[:consumed]
                buffer_pts += consumed / rate
            samples += count
            prior_end = pts + count / rate
        if source.read(1):
            raise AnalysisError("FFmpeg produced more PCM than ffprobe's decoded sample count")
    if not bands:
        raise AnalysisError("audio is shorter than a detector window")
    noise, peak = statistics.median(bands), max(bands)
    threshold = max(10 ** (-45 / 20), noise * 6, peak * 0.20)
    candidates = [i for i, band in enumerate(bands) if band >= threshold and ratios[i] >= 0.65]
    groups = []
    for index in candidates:
        if not groups or segments[index] != segments[groups[-1][-1]] or times[index] - times[groups[-1][-1]] > 0.010:
            groups.append([index])
        else:
            groups[-1].append(index)
    events = []
    for group in groups:
        begin, end = group[0], group[-1]
        duration = times[end] - times[begin] + size / rate
        if duration * 1000 < minimum_ms:
            continue
        if begin == 0 or end == len(times) - 1:
            raise AnalysisError("tone touches capture start/end; retain silence around all measured pulses")
        if duration > 0.200:
            raise AnalysisError("detected tone longer than 200ms; isolate the flash/tone test or reduce interference")
        events.append(dict(onset_pts_s=times[begin] + size / rate / 2,
            detector_window_start_s=times[begin], detector_window_end_s=times[begin] + size / rate,
            duration_ms=duration * 1000, peak_band_rms=max(bands[i] for i in group), segment=int(segments[begin])))
    return events, dict(sample_rate=rate, decoded_samples=samples, first_sample_pts_s=first_pts,
        last_sample_end_pts_s=prior_end, timestamp_gaps=gaps, tone_hz=frequency,
        detector="native-rate sine/cosine RMS projection; no causal filter",
        window_ms=size * 1000 / rate, hop_ms=hop * 1000 / rate, band_noise_rms=noise,
        band_peak_rms=peak, band_threshold_rms=threshold, minimum_tone_ms=minimum_ms)


def pair_events(optical, audio, expected, period, tolerance, max_offset, calibration, calibration_uncertainty):
    if len(optical) != len(audio) or len(optical) < 3:
        raise AnalysisError(f"cannot pair reliably: {len(optical)} flashes / {len(audio)} tones; require equal counts and at least 3")
    if expected is not None and len(optical) != expected:
        raise AnalysisError(f"expected {expected} pairs, detected {len(optical)}")
    for events, key, kind in ((optical, "first_bright_pts_s", "flash"), (audio, "onset_pts_s", "tone")):
        for left, right in zip(events, events[1:]):
            interval = right[key] - left[key]
            if abs(interval - period) > tolerance:
                raise AnalysisError(f"{kind} interval {interval:.6f}s differs from expected {period}s; possible missed/extra pulse")
    pairs = []
    for index, (light, sound) in enumerate(zip(optical, audio)):
        offset = sound["onset_pts_s"] - light["first_bright_pts_s"]
        if abs(offset) > max_offset:
            raise AnalysisError(f"pair {index} differs by {offset * 1000:.2f}ms; exceeds unambiguous pairing window")
        lower = sound["detector_window_start_s"] - light["first_bright_pts_s"]
        upper = sound["detector_window_end_s"] - light["previous_dark_pts_s"]
        pairs.append(dict(index=index, optical=light, audio=sound, raw_audio_minus_optical_ms=offset * 1000,
            raw_sampling_bounds_ms=[lower * 1000, upper * 1000],
            corrected_audio_minus_optical_ms=None if calibration is None else (offset - calibration) * 1000,
            corrected_sampling_bounds_ms=None if calibration is None else [(lower - calibration) * 1000, (upper - calibration) * 1000],
            corrected_bounds_with_manual_uncertainty_ms=None if calibration is None or calibration_uncertainty is None else
                [(lower - calibration) * 1000 - calibration_uncertainty, (upper - calibration) * 1000 + calibration_uncertainty]))
    return pairs


def analyze(args, report):
    ffmpeg, ffprobe = executable("ffmpeg", args.ffmpeg), executable("ffprobe", args.ffprobe)
    path = args.input.resolve()
    metadata = run_json([ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)])
    streams = metadata.get("streams", [])
    video = next((stream for stream in streams if stream.get("codec_type") == "video"), None)
    audio = next((stream for stream in streams if stream.get("codec_type") == "audio"), None)
    if video is None or audio is None:
        raise AnalysisError("capture must contain both video and audio")
    x, y, width, height = args.roi
    if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > video["width"] or y + height > video["height"]:
        raise AnalysisError(f"ROI outside encoded video {video['width']}x{video['height']}; autorotation is disabled")
    report["source"] = dict(file=str(path), bytes=path.stat().st_size, label=args.label,
        roi_encoded_pixels=args.roi, coordinate_space="encoded pixels; FFmpeg autorotation disabled",
        format_start_time=metadata.get("format", {}).get("start_time"),
        video_stream={key: video.get(key) for key in ("index", "codec_name", "time_base", "start_time", "width", "height", "avg_frame_rate", "r_frame_rate", "side_data_list")},
        audio_stream={key: audio.get(key) for key in ("index", "codec_name", "time_base", "start_time", "sample_rate")})
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    report["source"]["sha256"] = digest.hexdigest()
    report["ffmpeg"] = subprocess.check_output([ffmpeg, "-version"], text=True).splitlines()[0]
    video_frames = frame_probe(ffprobe, path, "v:0")
    audio_frames = frame_probe(ffprobe, path, "a:0")
    with tempfile.TemporaryDirectory(prefix="sync-", dir=args.output.parent) as folder:
        folder = Path(folder)
        mean_file, pcm_file = folder / "roi.gray", folder / "audio.f32"
        decode(ffmpeg, ["-copyts", "-noautorotate", "-i", str(path), "-map", "0:v:0", "-an", "-sn", "-dn",
            "-vf", f"crop={width}:{height}:{x}:{y}:exact=1,format=gray,scale=1:1:flags=area",
            "-fps_mode", "passthrough", "-c:v", "rawvideo", "-pix_fmt", "gray", "-f", "rawvideo"], mean_file)
        optical, video_info = optical_events(video_frames, mean_file.read_bytes(), args.flash_threshold)
        report["video"] = dict(**video_info, events=optical)
        decode(ffmpeg, ["-copyts", "-i", str(path), "-map", "0:a:0", "-vn", "-sn", "-dn",
            "-ac", "1", "-c:a", "pcm_f32le", "-f", "f32le"], pcm_file)
        tones, audio_info = audio_events(audio_frames, pcm_file, int(audio["sample_rate"]), args.tone_hz, args.tone_min_ms)
        report["audio"] = dict(**audio_info, events=tones)
    calibrated = args.calibration_video_contact is not None
    bias = args.calibration_audio_contact - args.calibration_video_contact if calibrated else None
    report["calibration"] = dict(applied=calibrated, video_contact_pts_s=args.calibration_video_contact,
        audio_contact_pts_s=args.calibration_audio_contact, capture_audio_minus_video_bias_ms=None if bias is None else bias * 1000,
        manual_contact_uncertainty_ms=args.calibration_uncertainty_ms,
        convention="corrected = recorded(audio - optical) - (clap_audio_contact - clap_video_contact)",
        requirement="same phone settings/geometry; use contact times on the calibration capture's common PTS axis")
    pairs = pair_events(optical, tones, args.expected_pairs, args.event_period, args.period_tolerance,
                        args.pair_window_ms / 1000, bias, args.calibration_uncertainty_ms)
    report["pairs"] = pairs
    offsets = [pair["raw_audio_minus_optical_ms"] for pair in pairs]
    corrected = None if bias is None else [value - bias * 1000 for value in offsets]
    report["summary"] = dict(pair_count=len(pairs), sign="positive means sound later than recorded light",
        raw_median_ms=statistics.median(offsets), raw_min_ms=min(offsets), raw_max_ms=max(offsets),
        corrected_median_ms=None if corrected is None else statistics.median(corrected),
        corrected_max_abs_ms=None if corrected is None else max(abs(value) for value in corrected),
        ideal_frame_resolution_ms={"30fps_one_frame": 1000 / 30, "30fps_half_frame": 500 / 30,
                                   "60fps_one_frame": 1000 / 60, "60fps_half_frame": 500 / 60},
        actual_max_flash_frame_interval_ms=max(event["frame_interval_ms"] for event in optical),
        bound_note="First-bright PTS is the nominal optical time; previous-dark/first-bright bracket is preserved. Audio onset uses the 4ms detector window center. Manual calibration uncertainty is unknown unless supplied; expanded bounds remain sampling bounds, not a camera exposure confidence interval.",
        capture_limits=["same phone capture mode and geometry required for calibration",
            "rolling shutter and exposure timing can move the ROI transition inside a camera frame",
            "manual clap contact pick and 1500Hz onset use different detection methods",
            "sound propagation/path differences are not removed by a different calibration geometry"],
        acceptance="not evaluated; synthetic tests and player internal PTS cannot establish physical 100ms acceptance")
    report["status"] = "analyzed"


def roi(value):
    try:
        result = [int(item) for item in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("ROI must be x,y,width,height integers") from error
    if len(result) != 4:
        raise argparse.ArgumentTypeError("ROI must contain x,y,width,height")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--roi", type=roi, required=True, help="x,y,width,height in encoded pixels (no autorotate)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", default="capture")
    parser.add_argument("--ffmpeg")
    parser.add_argument("--ffprobe")
    parser.add_argument("--synthetic", action="store_true", help="mark generated tests; never physical acceptance")
    parser.add_argument("--flash-threshold", type=float, help="ROI grayscale mean 0..255; default uses baseline/peak")
    parser.add_argument("--tone-hz", type=float, default=1500)
    parser.add_argument("--tone-min-ms", type=float, default=10)
    parser.add_argument("--expected-pairs", type=int, help="exact count; otherwise require >=3 and complete 1s cadence")
    parser.add_argument("--event-period", type=float, default=1.0)
    parser.add_argument("--period-tolerance", type=float, default=0.15)
    parser.add_argument("--pair-window-ms", type=float, default=400)
    parser.add_argument("--calibration-video-contact", type=float)
    parser.add_argument("--calibration-audio-contact", type=float)
    parser.add_argument("--calibration-uncertainty-ms", type=float,
                        help="additional manual contact-pick +/- uncertainty; keep separate from frame sampling bounds")
    args = parser.parse_args()
    if (args.calibration_video_contact is None) != (args.calibration_audio_contact is None):
        parser.error("provide both calibration contact times, or neither")
    numbers = [args.tone_hz, args.tone_min_ms, args.event_period, args.period_tolerance,
               args.pair_window_ms]
    numbers.extend(value for value in (args.flash_threshold, args.calibration_video_contact,
                                      args.calibration_audio_contact, args.calibration_uncertainty_ms) if value is not None)
    if any(not math.isfinite(value) for value in numbers) or min(args.tone_hz, args.tone_min_ms, args.event_period, args.pair_window_ms) <= 0:
        parser.error("numeric settings must be finite and durations/frequency positive")
    if args.period_tolerance < 0 or (args.calibration_uncertainty_ms is not None and args.calibration_uncertainty_ms < 0) or args.pair_window_ms >= args.event_period * 500:
        parser.error("pair window must be less than half the pulse period; uncertainties must be nonnegative")
    if args.flash_threshold is not None and not 0 <= args.flash_threshold <= 255:
        parser.error("flash threshold must be between 0 and 255")
    if args.expected_pairs is not None and args.expected_pairs < 3:
        parser.error("expected pairs must be at least 3")
    args.output = args.output.resolve()
    if args.output == args.input.resolve():
        parser.error("report output must differ from the input capture")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schema=1, kind="synthetic" if args.synthetic else "real_capture", status="error", pairs=[],
        note="Measures captured optical/audio events; no board timestamp substitutes for physical capture.")
    report["settings"] = {name: getattr(args, name) for name in
        ("roi", "flash_threshold", "tone_hz", "tone_min_ms", "expected_pairs", "event_period",
         "period_tolerance", "pair_window_ms", "calibration_video_contact", "calibration_audio_contact",
         "calibration_uncertainty_ms")}
    try:
        analyze(args, report)
    except (AnalysisError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        report["error"] = str(error)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    if report["status"] == "error":
        print(f"SYNC_ANALYSIS kind={report['kind']} ERROR={report['error']} report={args.output}", file=sys.stderr)
        return 2
    summary = report["summary"]
    print(f"SYNC_ANALYSIS kind={report['kind']} pairs={summary['pair_count']} raw_median_ms={summary['raw_median_ms']:.3f} "
          f"corrected_median_ms={summary['corrected_median_ms']} physical_acceptance=not-evaluated report={args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
