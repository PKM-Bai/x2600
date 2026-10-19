"""Build and validate a 30-minute motion/flash/tone AVI by copying a 30-second loop."""
import argparse
from array import array
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[3]
RATE = 48000
FPS = 30
PEAK = 8192
TONE_HZ = 1500
PERIOD_SAMPLES = RATE // FPS


def progress(message):
    print(message, flush=True)


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def pcm_bytes(seconds, tail=0):
    samples = array('h', [0]) * (seconds * RATE + tail)
    starts = list(range(RATE // 2, seconds * RATE, RATE))
    if tail:
        starts.append(seconds * RATE)
    for start in starts:
        count = min(PERIOD_SAMPLES, len(samples) - start)
        for index in range(count):
            samples[start + index] = round(PEAK * math.cos(2 * math.pi * TONE_HZ * index / RATE))
    assert max(abs(value) for value in samples) == PEAK
    if sys.byteorder != 'little': samples.byteswap()
    return samples.tobytes(), starts


def write_wav(path, pcm):
    with wave.open(str(path), 'wb') as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(pcm)


def compile_parser(media_test, output):
    executable = output / ('player_media_test.exe' if os.name == 'nt' else 'player_media_test')
    common = ROOT / 'projects/halley7m-player/common'
    if os.name == 'nt':
        environment = media_test.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
        command = [compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS',
                   '/I' + str(common), str(common / 'player_media.c'), str(ROOT / 'tests/player_media_test.c'),
                   '/Fe:' + str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True)
    else:
        subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-D_FILE_OFFSET_BITS=64', '-D_POSIX_C_SOURCE=200809L', '-I', str(common),
            str(common / 'player_media.c'), str(ROOT / 'tests/player_media_test.c'), '-o', str(executable)],
            cwd=output, check=True)
    return executable


def scan_copy(media, path, pcm_period, frame_hashes=None):
    """Validate every chunk and compare payloads without decoding 30min of raw video.

    Each base AU is already profile/NAL checked and decoded. Proving every long
    AU is byte-identical, in the same loop order, proves its bitstream contract;
    the native parser independently checks AU framing and complete durations.
    """
    size = path.stat().st_size
    if size >= 1 << 32: raise ValueError('FAT32 requires less than 4GiB')
    hashes = []
    frames = audio_bytes = riffs = maximum = 0
    with path.open('rb') as handle:
        first = media.read_exact(handle, 0, 12)
        if first[:4] != b'RIFF' or first[8:] != b'AVI ': raise ValueError('Expected AVI RIFF')
        streams = None
        for tag, position, length in media.chunks(handle, 12, 8 + media.u32(first, 4)):
            if tag == b'LIST' and media.read_exact(handle, position, 4) == b'hdrl':
                streams = media.stream_headers(handle, position + 4, position + length)
                break
        if not streams or len(streams) != 2: raise ValueError('Expected two stream headers')
        video = next(index for index, stream in enumerate(streams) if stream['type'] == b'vids')
        audio = next(index for index, stream in enumerate(streams) if stream['type'] == b'auds')
        video_tag, audio_tag = f'{video:02d}dc'.encode(), f'{audio:02d}wb'.encode()

        def movi(start, end, depth=0):
            nonlocal frames, audio_bytes, maximum
            if depth >= 7: raise ValueError('RIFF nesting limit')
            for tag, offset, length in media.chunks(handle, start, end):
                if tag == b'LIST' and media.read_exact(handle, offset, 4) == b'rec ':
                    movi(offset + 4, offset + length, depth + 1)
                elif tag == video_tag:
                    if not 0 < length <= media.VIDEO_MAX: raise ValueError('AU size limit')
                    payload = media.read_exact(handle, offset, length)
                    value = hashlib.sha256(payload).digest()
                    if frame_hashes is None:
                        hashes.append(value)
                    elif value != frame_hashes[frames % len(frame_hashes)]:
                        raise ValueError(f'Video stream-copy mismatch at frame {frames}')
                    maximum = max(maximum, length)
                    frames += 1
                elif tag == audio_tag:
                    if length & 1: raise ValueError('Odd PCM payload')
                    payload = media.read_exact(handle, offset, length)
                    begin = audio_bytes % len(pcm_period)
                    expected = pcm_period[begin:begin + length]
                    if len(expected) < length:
                        remaining = length - len(expected)
                        expected += (pcm_period * (remaining // len(pcm_period) + 1))[:remaining]
                    if payload != expected: raise ValueError(f'PCM phase/payload mismatch at sample {audio_bytes // 2}')
                    audio_bytes += length

        position = 0
        while position < size:
            header = media.read_exact(handle, position, 12)
            if header[:4] != b'RIFF' or header[8:] != (b'AVI ' if not riffs else b'AVIX'):
                raise ValueError('Expected AVI/AVIX')
            end = position + 8 + media.u32(header, 4)
            if end > size or end < position + 12: raise ValueError('RIFF extent error')
            for tag, offset, length in media.chunks(handle, position + 12, end):
                if tag == b'LIST' and media.read_exact(handle, offset, 4) == b'movi':
                    movi(offset + 4, offset + length)
            riffs += 1
            position = end + (end & 1)
        if frames != streams[video]['count'] or audio_bytes // 2 != streams[audio]['count']:
            raise ValueError('Stream lengths disagree with actual packets')
        if frames * PERIOD_SAMPLES * 2 != audio_bytes:
            raise ValueError('Audio/video duration mismatch')
    return dict(video_frames=frames, audio_samples=audio_bytes // 2, riff_count=riffs,
                max_video_packet=maximum, seconds=frames / FPS,
                all_au_equal_verified_base=frame_hashes is not None, all_pcm_exact=True), hashes


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument('--ffmpeg')
    arguments.add_argument('--font', type=Path)
    arguments.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture/acceptance')
    arguments.add_argument('--seconds', type=int, default=1800)
    arguments.add_argument('--base-seconds', type=int, default=30)
    args = arguments.parse_args()
    if args.base_seconds < 1 or args.seconds < args.base_seconds or args.seconds % args.base_seconds:
        raise ValueError('Total seconds must be a positive whole number of base loops')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('prepare_media', ROOT / 'scripts/prepare-player-media.py')
    media = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(media)
    spec = importlib.util.spec_from_file_location('media_test', ROOT / 'scripts/test-player-media.py')
    media_test = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(media_test)
    ffmpeg = media.find_ffmpeg(args.ffmpeg)
    font = args.font or (Path('C:/Windows/Fonts/consola.ttf') if os.name == 'nt' else
                         Path('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf'))
    if not font.is_file(): raise ValueError('A readable font is required for frame labels; pass --font')
    pcm, starts = pcm_bytes(args.base_seconds)
    source_wav = output / 'acceptance-source.wav'
    write_wav(source_wav, pcm)
    # NUT retains exact H264 PTS and 48kHz PCM timestamps. H264-in-AVI source
    # packets may expose only DTS, which is unsuitable for the preparer's
    # packet-based input-end calculation. The deliverable remains standard AVI.
    source = output / 'acceptance-source.nut'
    base = output / f'acceptance-base-{args.base_seconds}s.avi'
    long_video = output / ('acceptance-30min.avi' if args.seconds == 1800 else f'acceptance-{args.seconds}s.avi')
    frame_count = args.base_seconds * FPS
    font_name = str(font.resolve()).replace('\\', '/').replace(':', r'\:')
    video_filter = (
        f"drawtext=fontfile='{font_name}':text='LOOP FRAME %{{n}} / {frame_count}':"
        "fontcolor=white:fontsize=36:x=24:y=24:box=1:boxcolor=black@0.6:boxborderw=8,"
        "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='eq(mod(n,30),15)'")
    progress(f'Generating {args.base_seconds}s high-motion source, one white/tone pulse per second')
    with (output / 'generation.log').open('w', encoding='utf-8') as log:
        subprocess.run([str(ffmpeg), '-hide_banner', '-y', '-f', 'lavfi', '-i',
            f'testsrc2=size=1280x720:rate=30:duration={args.base_seconds}', '-i', str(source_wav),
            '-map', '0:v', '-map', '1:a', '-vf', video_filter,
            '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '18', '-bf', '0', '-refs', '1', '-g', '30',
            '-pix_fmt', 'yuv420p', '-c:a', 'pcm_s16le', '-t', str(args.base_seconds), '-f', 'nut', str(source)],
            stdout=log, stderr=subprocess.STDOUT, check=True)
        progress('Transcoding base with the production fixed profile')
        subprocess.run([sys.executable, str(ROOT / 'scripts/prepare-player-media.py'), str(source), str(base),
                        '--ffmpeg', str(ffmpeg), '--overwrite'], stdout=log, stderr=subprocess.STDOUT, check=True)
        progress(f'Stream-copy looping base to {args.seconds}s (no long re-encode)')
        subprocess.run([str(ffmpeg), '-hide_banner', '-y', '-stream_loop',
            str(args.seconds // args.base_seconds - 1), '-i', str(base), '-map', '0:v', '-map', '0:a',
            '-c', 'copy', '-t', str(args.seconds), '-f', 'avi', str(long_video)],
            stdout=log, stderr=subprocess.STDOUT, check=True)
    progress('Decoding base to verify bright-frame phase and moving image content')
    pixels = subprocess.check_output([str(ffmpeg), '-v', 'error', '-i', str(base), '-map', '0:v',
        '-vf', 'format=gray,scale=16:16:flags=area', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'])
    if len(pixels) != frame_count * 256: raise ValueError('Base decoded frame count mismatch')
    pictures = [pixels[offset:offset + 256] for offset in range(0, len(pixels), 256)]
    bright = [index for index, picture in enumerate(pictures) if sum(picture) >= 250 * 256]
    expected_bright = list(range(15, frame_count, FPS))
    if bright != expected_bright: raise ValueError(f'Flash phase mismatch: {bright}')
    distinct = len({picture for index, picture in enumerate(pictures) if index not in bright})
    if distinct < args.base_seconds * 5: raise ValueError('Motion pattern unexpectedly static')
    decoded_pcm = subprocess.check_output([str(ffmpeg), '-v', 'error', '-i', str(base), '-map', '0:a',
                                           '-f', 's16le', '-acodec', 'pcm_s16le', '-'])
    if decoded_pcm != pcm: raise ValueError('Base PCM amplitude or pulse phase changed')
    base_contract = media.validate_media(base)
    media.verify_codecs(base, ffmpeg)
    base_scan, hashes = scan_copy(media, base, pcm)
    progress('Checking every long AVI packet: exact repeated AU hashes, PCM phase and declared lengths')
    long_scan, _ = scan_copy(media, long_video, pcm, hashes)
    media.verify_codecs(long_video, ffmpeg)
    if long_scan['video_frames'] != args.seconds * FPS or long_scan['audio_samples'] != args.seconds * RATE:
        raise ValueError('Long output is not exactly the requested duration')
    short_pcm, short_starts = pcm_bytes(2, 17)
    short_wav = output / 'acceptance-tail-96017.wav'
    write_wav(short_wav, short_pcm)
    short_contract = media.validate_media(short_wav)
    media.verify_codecs(short_wav, ffmpeg)
    progress('Building/running production native parser on long AVI and short-tail WAV')
    parser = compile_parser(media_test, output)
    native = {}
    for path in (base, long_video, short_wav):
        result = subprocess.check_output([str(parser), str(path)], text=True)
        native[path.name] = json.loads(result)
    if native[long_video.name]['frames'] != args.seconds * FPS or native[long_video.name]['samples'] != args.seconds * RATE:
        raise ValueError('Native long parser count mismatch')
    if native[short_wav.name]['samples'] != 96017: raise ValueError('Native tail sample count mismatch')
    progress('Hashing acceptance artifacts and writing phase/contract manifest')
    manifest = dict(
        base_seconds=args.base_seconds, total_seconds=args.seconds, fps='30/1', sample_rate=RATE,
        geometry='720x1280 encoded / 1280x720 landscape', video='H264 Baseline L3.1 ref1 B0 CABAC0 GOP30 AUD repeated SPS/PPS',
        color='BT.601 full', pcm_format='S16LE mono', pcm_peak=PEAK, tone_hz=TONE_HZ,
        pulse_phase=dict(video_frame_mod_30=15, seconds_mod_1=0.5, samples_per_pulse=PERIOD_SAMPLES,
                         first_sample=RATE // 2, sample_stride=RATE),
        label=f'LOOP FRAME n / {frame_count}; label and motion repeat every {args.base_seconds}s',
        phone_check_seconds=[second for second in (60, 900, 1740) if second < args.seconds],
        phone_check_note='At 1/15/29min, every second has white+tone at +0.5s; display label resets at each base-loop boundary.',
        base_first_tone_samples=starts, base_bright_frames=bright, base_distinct_nonflash_16x16=distinct,
        base_contract=base_contract, base_scan=base_scan, long_scan=long_scan,
        short_wav=dict(samples=96017, final_valid_period_samples=17, tone_start_samples=short_starts,
                       final_17_sample_period_contains_tone=True, contract=short_contract),
        native=native, files={path.name: dict(bytes=path.stat().st_size, sha256=digest(path))
                             for path in (base, long_video, short_wav)},
        tf_status='PC fixtures only; physical TF copy/read/playback has not been validated.')
    (output / 'acceptance-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    progress(json.dumps(dict(directory=str(output), long=long_scan, files=manifest['files'],
                             pcm_peak=PEAK, pulse_phase=manifest['pulse_phase']), ensure_ascii=False))


if __name__ == '__main__':
    main()
