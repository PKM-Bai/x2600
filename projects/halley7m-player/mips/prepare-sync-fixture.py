"""Generate and verify a three-second, peak-8192 flash/tone RAM diagnostic AVI."""
import argparse
from array import array
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[3]
FLASH_FRAMES = (15, 45, 75)
SAMPLE_RATE = 48000
FPS = 30
SAMPLES_PER_FRAME = SAMPLE_RATE // FPS
PEAK = 8192
TONE_HZ = 1500


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument('--ffmpeg')
    arguments.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture')
    args = arguments.parse_args()
    spec = importlib.util.spec_from_file_location('prepare_player_media', ROOT / 'scripts/prepare-player-media.py')
    media = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(media)
    ffmpeg = media.find_ffmpeg(args.ffmpeg)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    pcm = array('h', [0]) * (SAMPLE_RATE * 3)
    for frame in FLASH_FRAMES:
        start = frame * SAMPLES_PER_FRAME
        for sample in range(SAMPLES_PER_FRAME):
            # Cosine starts at its positive peak, so the first audible sample
            # has exactly the same timestamp as the first white video frame.
            pcm[start + sample] = round(PEAK * math.cos(2 * math.pi * TONE_HZ * sample / SAMPLE_RATE))
    if sys.byteorder != 'little':
        pcm.byteswap()
    source_pcm = pcm.tobytes()
    audio = output / 'sync-source.wav'
    with wave.open(str(audio), 'wb') as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(source_pcm)
    source = output / 'sync-source.avi'
    destination = output / 'sync-flash-tone.avi'
    enable = '+'.join(f'eq(n,{frame})' for frame in FLASH_FRAMES)
    video = ('color=c=black:size=1280x720:rate=30:duration=3,'
             f"drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='{enable}'")
    subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
                    '-f', 'lavfi', '-i', video, '-i', str(audio), '-map', '0:v', '-map', '1:a',
                    '-c:v', 'ffv1', '-level', '3', '-pix_fmt', 'yuv420p',
                    '-colorspace', 'smpte170m', '-color_range', 'tv',
                    '-c:a', 'pcm_s16le', '-ar', '48000', '-ac', '1', '-t', '3', '-f', 'avi', str(source)], check=True)
    media.convert(source, destination, ffmpeg, True)
    contract = media.validate_media(destination)
    media.verify_codecs(destination, ffmpeg)
    if contract.get('video_frames') != 90 or contract.get('audio_samples') != 144000:
        raise ValueError(f'Unexpected media duration: {contract}')
    if destination.stat().st_size > 6 * 1024 * 1024:
        raise ValueError('Diagnostic AVI alone exceeds the 6MiB RAM package budget')
    decoded_pcm = subprocess.check_output([str(ffmpeg), '-v', 'error', '-i', str(destination),
                                           '-map', '0:a:0', '-f', 's16le', '-acodec', 'pcm_s16le', '-'])
    if decoded_pcm != source_pcm:
        raise ValueError('PCM changed during preparation or flash/tone timestamps shifted')
    levels = subprocess.check_output([str(ffmpeg), '-v', 'error', '-i', str(destination),
        '-map', '0:v:0', '-vf', 'format=gray,scale=1:1:flags=area', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'])
    if len(levels) != 90:
        raise ValueError(f'Decoded {len(levels)} video frames, expected 90')
    bright = [index for index, value in enumerate(levels) if value >= 250]
    if bright != list(FLASH_FRAMES) or any(value > 5 for index, value in enumerate(levels) if index not in FLASH_FRAMES):
        raise ValueError(f'Flash frame check failed: {bright}, levels={list(levels)}')
    decoded_samples = array('h')
    decoded_samples.frombytes(decoded_pcm)
    if sys.byteorder != 'little': decoded_samples.byteswap()
    measured_peak = max(abs(value) for value in decoded_samples)
    expected_starts = [frame * SAMPLES_PER_FRAME for frame in FLASH_FRAMES]
    measured_starts = [index for index, value in enumerate(decoded_samples)
                       if value and (index == 0 or not decoded_samples[index - 1]) and index % SAMPLES_PER_FRAME == 0]
    if measured_peak != PEAK or measured_starts != expected_starts:
        raise ValueError(f'Tone pulse check failed: peak={measured_peak}, starts={measured_starts}')
    result = dict(file=str(destination), bytes=destination.stat().st_size,
                  sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
                  seconds=3, flash_frames=bright, tone_start_samples=expected_starts,
                  tone_samples=SAMPLES_PER_FRAME, tone_hz=TONE_HZ, pcm_peak=measured_peak,
                  pcm_exact=True, contract=contract,
                  note='Explicit RAM diagnostic only; this does not validate physical TF storage.')
    (output / 'sync-flash-tone.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))


if __name__ == '__main__':
    main()
