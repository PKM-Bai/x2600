"""Create and verify a single batch of 30-second player format board fixtures.

Outputs stay under build/player-fixture/formats. No serial port, TF card, or
audio hardware is opened. Compression padding is reported separately from
the known source sample count; a nominal 30 seconds is never assumed exact
for headerless ADTS. Requires the pinned FFmpeg/FFprobe and a host C compiler.
"""
from __future__ import annotations

import argparse
from array import array
from fractions import Fraction
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1]
FPS, PEAK = 30, 8192


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def progress(message):
    print(message, flush=True)


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True)
    if result.returncode:
        raise RuntimeError(f'command failed: {command}\n' + result.stderr.decode(errors='replace')[-12000:])
    return result.stdout


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def write_source_audio(path, rate, channels, seconds):
    """Integer-second repeating, click-ramped channel-identifying tone pulses."""
    period = array('h')
    ramp = max(1, rate // 200)  # Five milliseconds at either edge.
    for index in range(rate):
        for channel in range(channels):
            start = 0 if channel == 0 else rate // 2
            local, count = index - start, rate // 4
            if 0 <= local < count:
                gain = min(1.0, local / ramp, (count - 1 - local) / ramp)
                hz = 997 if channel == 0 else 613
                value = round(PEAK * gain * math.sin(2 * math.pi * hz * local / rate))
            else:
                value = 0
            period.append(value)
    actual_peak = max(map(abs, period))
    assert actual_peak == PEAK
    if sys.byteorder != 'little':
        period.byteswap()
    pcm = period.tobytes() * seconds
    with wave.open(str(path), 'wb') as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(pcm)
    return {
        'file': path.name, 'sample_rate': rate, 'channels': channels,
        'source_frames': rate * seconds, 'source_interleaved_samples': rate * seconds * channels,
        'pcm_bytes': len(pcm), 'pcm_peak': actual_peak,
        'pcm_sha256': hashlib.sha256(pcm).hexdigest(), 'file_sha256': digest(path),
        'pattern': 'L/mono:997Hz at [0,250)ms; R:613Hz at [500,750)ms, each second; 5ms ramps',
    }


def make_picture_source(ffmpeg, output, audio, seconds):
    pattern = load('player_probe_picture', ROOT / 'scripts/make-player-probe-media.py')
    source = output / '_sources/pattern-lossless.mkv'
    command = [ffmpeg, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
               '-video_size', '1280x720', '-framerate', FPS, '-i', 'pipe:0',
               '-i', audio, '-map', '0:v:0', '-map', '1:a:0',
               '-c:v', 'ffv1', '-level', '3', '-threads:v', '1', '-pix_fmt', 'bgr0',
               '-c:a', 'pcm_s16le', '-map_metadata', '-1', '-fflags', '+bitexact',
               '-flags:v', '+bitexact', '-flags:a', '+bitexact', '-t', seconds, source]
    rgb_digest = hashlib.sha256()
    with (output / '_reports/source-encode.log').open('wb') as log:
        process = subprocess.Popen(list(map(str, command)), stdin=subprocess.PIPE,
                                   stdout=subprocess.DEVNULL, stderr=log)
        try:
            for index in range(seconds * FPS):
                pixels = pattern.picture(index)
                rgb_digest.update(pixels)
                process.stdin.write(pixels)
                if (index + 1) % 150 == 0:
                    progress(f'RGB pattern {index + 1}/{seconds * FPS} frames')
        finally:
            process.stdin.close()
        if process.wait():
            raise RuntimeError('lossless source encode failed; see _reports/source-encode.log')
    return source, {
        'file': source.relative_to(output).as_posix(), 'file_sha256': digest(source),
        'video_frames': seconds * FPS, 'width': 1280, 'height': 720, 'fps': '30/1',
        'rgb24_stream_bytes': seconds * FPS * 1280 * 720 * 3,
        'rgb24_stream_sha256': rgb_digest.hexdigest(),
        'audio_source': audio.relative_to(output).as_posix(), 'audio_frames': seconds * 48000,
        'audio_rate': 48000, 'audio_channels': 2,
        'picture': f'eight color bars, 16 gray steps, asymmetric corner marks, 000..{seconds * FPS - 1:03d} frame number, moving white marker',
    }


def probe(ffprobe, path):
    return json.loads(run([ffprobe, '-v', 'error', '-show_streams', '-show_format',
                           '-show_packets', '-of', 'json', path]))


def pcm_stats(ffmpeg, path, channels):
    pcm = run([ffmpeg, '-v', 'error', '-i', path, '-map', '0:a:0',
               '-c:a', 'pcm_s16le', '-f', 's16le', '-'])
    if not pcm or len(pcm) % (channels * 2):
        raise ValueError(f'{path.name}: invalid decoded PCM extent')
    samples = array('h')
    samples.frombytes(pcm)
    if sys.byteorder != 'little':
        samples.byteswap()
    return {
        'ffmpeg_decoded_frames': len(pcm) // (channels * 2),
        'ffmpeg_decoded_interleaved_samples': len(pcm) // 2,
        'ffmpeg_decoded_pcm_sha256': hashlib.sha256(pcm).hexdigest(),
        'ffmpeg_decoded_peak': max(map(abs, samples)),
    }


def check_audio(path, meta, rate, channels, seconds):
    audio = next(stream for stream in meta['streams'] if stream['codec_type'] == 'audio')
    assert int(audio['sample_rate']) == rate and int(audio['channels']) == channels
    assert audio['codec_name'] == ('mp3' if path.suffix == '.mp3' else 'aac')
    if audio['codec_name'] == 'aac':
        assert audio['profile'] == 'LC'
    packets = [packet for packet in meta['packets'] if packet['codec_type'] == 'audio']
    assert packets and all(int(packet['size']) > 0 for packet in packets)
    skip = []
    for packet in packets:
        skip.extend(item for item in packet.get('side_data_list', []) if item.get('side_data_type') == 'Skip Samples')
    result = {
        'source_rate': rate, 'source_channels': channels, 'source_frames': rate * seconds,
        'source_interleaved_samples': rate * seconds * channels, 'audio_packets': len(packets),
        'codec': audio['codec_name'], 'time_base': audio['time_base'],
        'stream_duration_ts': int(audio['duration_ts']) if 'duration_ts' in audio else None,
        'stream_duration': audio.get('duration'), 'skip_side_data': skip,
        'coded_frames_assuming_lc1024_or_mpeg1layer3_1152': len(packets) * (1152 if audio['codec_name'] == 'mp3' else 1024),
    }
    if path.suffix in ('.m4a', '.mp4'):
        # These fixtures are encoded directly from the exact PCM source and
        # have container edit metadata. Headerless ADTS cannot express this.
        expected = Fraction(rate * seconds, rate)
        actual = int(audio['duration_ts']) * Fraction(audio['time_base'])
        assert actual == expected, (path, actual, expected)
        result['container_presentation_frames'] = rate * seconds
        result['expected_48k_after_container_trim'] = 48000 * seconds
    return result


def check_video(ffmpeg, path, meta, seconds, reports):
    video = next(stream for stream in meta['streams'] if stream['codec_type'] == 'video')
    assert video['codec_name'] == 'h264'
    assert video['profile'] in ('Baseline', 'Constrained Baseline') and video['level'] == 31
    assert video['width'] == 1280 and video['height'] == 720
    assert video['has_b_frames'] == 0 and video.get('refs', 1) == 1
    assert video['r_frame_rate'] == '30/1' and video['avg_frame_rate'] == '30/1'
    assert video['pix_fmt'] in ('yuv420p', 'yuvj420p')
    assert video['color_space'] == 'smpte170m' and video['color_range'] == 'pc'
    packets = [packet for packet in meta['packets'] if packet['codec_type'] == 'video']
    assert len(packets) == seconds * FPS
    assert max(int(packet['size']) for packet in packets) <= 512 * 1024
    tb = Fraction(video['time_base'])
    for index, packet in enumerate(packets):
        assert int(packet['pts']) * tb == Fraction(index, FPS)
        assert packet['pts'] == packet['dts']
        assert int(packet['duration']) * tb == Fraction(1, FPS)
        assert ('K' in packet['flags']) == (index % FPS == 0)
    hashes = run([ffmpeg, '-v', 'error', '-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough',
                  '-f', 'framemd5', '-']).decode()
    frame_hashes = [line.rsplit(',', 1)[-1].strip() for line in hashes.splitlines() if not line.startswith('#')]
    assert len(frame_hashes) == seconds * FPS and len(set(frame_hashes)) > seconds * 10
    (reports / (path.stem + '.framemd5')).write_text(hashes, encoding='utf-8')
    trace = subprocess.run(list(map(str, [ffmpeg, '-v', 'info', '-i', path, '-map', '0:v:0',
                                         '-frames:v', '1', '-c:v', 'copy', '-bsf:v', 'trace_headers',
                                         '-f', 'null', '-'])), capture_output=True, check=True).stderr.decode(errors='replace')
    (reports / (path.stem + '.trace-headers.log')).write_text(trace, encoding='utf-8')
    for field, expected in (('profile_idc', 66), ('level_idc', 31), ('max_num_ref_frames', 1),
                            ('entropy_coding_mode_flag', 0), ('frame_mbs_only_flag', 1)):
        values = [int(value) for value in re.findall(r'\b' + field + r'\s+[^\r\n]*=\s*(\d+)', trace)]
        assert values and all(value == expected for value in values), (path, field, values)
    return {
        'video_frames': len(packets), 'width': 1280, 'height': 720, 'fps': '30/1',
        'profile': video['profile'], 'level': 31, 'refs': video.get('refs', 1), 'b_frames': 0,
        'gop_frames': 30, 'matrix': 'BT.601/smpte170m', 'range': 'full/pc',
        'max_video_source_bytes': max(int(packet['size']) for packet in packets),
        'frame_hash_list_sha256': hashlib.sha256('\n'.join(frame_hashes).encode()).hexdigest(),
    }


def native_mp4_check(executable, path, output):
    prefix = output / '_reports' / path.stem
    log = run([executable, path, prefix]).decode()
    (output / '_reports' / (path.stem + '.native.jsonl')).write_text(log, encoding='utf-8')
    records = [json.loads(line) for line in log.splitlines()]
    info = records[0]['info']
    done = records[-1]['done']
    video = [row['packet'] for row in records if row.get('packet', {}).get('kind') == 1]
    return {
        'info': info, 'packets': done,
        'max_normalized_video_bytes': max((packet['normalized'] for packet in video), default=0),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, default=ROOT / 'tools/ffmpeg/bin/ffmpeg.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture/formats')
    parser.add_argument('--seconds', type=int, default=30, help='1..33; 30 is the board batch')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cl' if os.name == 'nt' else 'gcc'))
    args = parser.parse_args()
    if not 1 <= args.seconds <= 33:
        parser.error('--seconds must be 1..33 (three-digit frame numbers)')
    ffmpeg = args.ffmpeg.resolve()
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    output = args.output.resolve()
    for folder in (output, output / '_sources', output / '_reports', output / '_host'):
        folder.mkdir(parents=True, exist_ok=True)
    ff = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y']
    report = {'schema': 1, 'seconds': args.seconds, 'pcm_source_peak': PEAK,
              'ffmpeg': run([ffmpeg, '-version']).decode().splitlines()[0],
              'sources': [], 'files': [], 'status': 'building',
              'note': 'PC fixtures and validation only; no hardware audio, TF, rotation speed, or playback claim.'}
    sources = {}
    for rate in (44100, 48000):
        for channels in (1, 2):
            path = output / '_sources' / f'source-{rate}-{channels}ch.wav'
            source = write_source_audio(path, rate, channels, args.seconds)
            source['file'] = path.relative_to(output).as_posix()
            report['sources'].append(source)
            sources[rate, channels] = path
    progress('Four exact S16 PCM sources created')

    products = []
    for rate in (44100, 48000):
        for channels in (1, 2):
            source = sources[rate, channels]
            base = f'{rate}-{channels}ch-{args.seconds}s'
            for suffix in ('.mp3', '.aac', '.m4a'):
                path = output / ('format-' + base + suffix)
                command = ff + ['-i', source, '-map', '0:a:0', '-map_metadata', '-1',
                                 '-threads:a', '1', '-fflags', '+bitexact', '-flags:a', '+bitexact']
                if suffix == '.mp3':
                    command += ['-c:a', 'libmp3lame', '-b:a', '128k' if channels == 1 else '192k', '-write_xing', '1']
                else:
                    command += ['-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '96k' if channels == 1 else '128k']
                    if suffix == '.aac':
                        command += ['-f', 'adts']
                    else:
                        command += ['-movflags', '+faststart']
                run(command + [path])
                products.append((path, rate, channels))
                progress('Encoded ' + path.name)

    source_video, video_source = make_picture_source(ffmpeg, output, sources[48000, 2], args.seconds)
    report['sources'].append(video_source)
    video48 = output / f'format-landscape-1280x720-30fps-48000-2ch-{args.seconds}s.mp4'
    command = ff + ['-i', source_video, '-map', '0:v:0', '-map', '0:a:0', '-map_metadata', '-1',
                    '-vf', 'scale=out_color_matrix=bt601:out_range=full,format=yuv420p,setsar=1',
                    '-c:v', 'libx264', '-preset', 'medium', '-profile:v', 'baseline', '-level:v', '3.1',
                    '-threads:v', '1', '-refs', '1', '-bf', '0', '-g', '30', '-keyint_min', '30', '-sc_threshold', '0',
                    '-x264-params', 'cabac=0:ref=1:bframes=0:open-gop=0:force-cfr=1:repeat-headers=1:aud=1',
                    '-b:v', '2M', '-maxrate', '3M', '-bufsize', '3M',
                    '-colorspace', 'smpte170m', '-color_primaries', 'smpte170m', '-color_trc', 'smpte170m', '-color_range', 'pc',
                    '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '128k', '-ar', '48000', '-ac', '2', '-threads:a', '1',
                    '-t', args.seconds, '-fflags', '+bitexact', '-flags:a', '+bitexact', '-movflags', '+faststart', video48]
    run(command)
    products.append((video48, 48000, 2))
    progress('Encoded ' + video48.name)
    video44 = output / f'format-landscape-1280x720-30fps-44100-2ch-{args.seconds}s.mp4'
    run(ff + ['-i', video48, '-i', sources[44100, 2], '-map', '0:v:0', '-map', '1:a:0',
              '-c:v', 'copy', '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '128k', '-threads:a', '1',
              '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact', '-t', args.seconds, video44])
    products.append((video44, 44100, 2))
    progress('Remuxed video with independent 44.1k stereo source: ' + video44.name)

    mp4_test = load('player_mp4_fixture_host', ROOT / 'scripts/test-player-mp4.py')
    executable = mp4_test.compile_host(output / '_host', args.cc)
    for path, rate, channels in products:
        meta = probe(ffprobe, path)
        write_json(output / '_reports' / (path.name + '.ffprobe.json'), meta)
        entry = {'file': path.name, 'bytes': path.stat().st_size, 'sha256': digest(path),
                 **check_audio(path, meta, rate, channels, args.seconds), **pcm_stats(ffmpeg, path, channels)}
        # FFmpeg gapless MP3 honors the generated Xing/LAME delay/padding.
        if path.suffix == '.mp3':
            assert entry['ffmpeg_decoded_frames'] == rate * args.seconds
        if path.suffix in ('.mp4', '.m4a'):
            entry['native_demux'] = native_mp4_check(executable, path, output)
            assert entry['native_demux']['info']['audio']['samples'] == entry['audio_packets']
        if path.suffix == '.mp4':
            entry.update(check_video(ffmpeg, path, meta, args.seconds, output / '_reports'))
            assert entry['native_demux']['packets']['video'] == args.seconds * FPS
            assert entry['native_demux']['max_normalized_video_bytes'] <= 512 * 1024
        report['files'].append(entry)
        write_json(output / 'manifest.json', report)
        progress(f"Verified {path.name}: source={entry['source_frames']} decoded={entry['ffmpeg_decoded_frames']} frames/ch")

    references = []
    for index in (0, args.seconds * FPS // 2, args.seconds * FPS - 1):
        # Same coded video in both MP4s. Visible 1280-byte NV12 rows, with the
        # same full-range conversion used for board CRC references.
        frame = run([ffmpeg, '-v', 'error', '-i', video48, '-map', '0:v:0',
                     '-vf', f'select=eq(n\\,{index}),scale=in_range=full:out_range=full,format=nv12',
                     '-frames:v', '1', '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'])
        assert len(frame) == 1280 * 720 * 3 // 2
        y_bytes = 1280 * 720
        frame_path = output / '_reports' / f'frame-{index:03d}.nv12'
        frame_path.write_bytes(frame)
        references.append({'frame_index': index, 'file': frame_path.relative_to(output).as_posix(),
                           'format': 'NV12', 'width': 1280, 'height': 720, 'stride': 1280,
                           'sha256': hashlib.sha256(frame).hexdigest(),
                           'y_crc32': f'{zlib.crc32(frame[:y_bytes]) & 0xffffffff:08x}',
                           'uv_crc32': f'{zlib.crc32(frame[y_bytes:]) & 0xffffffff:08x}'})
    run(ff + ['-i', video48, '-frames:v', '1', output / '_reports/preview-frame000.png'])
    report['nv12_references'] = references
    hashes = [item['frame_hash_list_sha256'] for item in report['files'] if item['file'].endswith('.mp4')]
    assert hashes[0] == hashes[1]
    report['status'] = 'pc-verified'
    report['copy_files'] = [entry['file'] for entry in report['files']]
    report['total_copy_bytes'] = sum(entry['bytes'] for entry in report['files'])
    write_json(output / 'manifest.json', report)
    lines = ['# 格式板测样片', '', f'本批次 {len(products)} 个媒体文件，源长度 {args.seconds} 秒。', '',
             '仅复制本目录顶层的 MP3/AAC/M4A/MP4；_sources、_reports、_host 是 PC 证据，不需要复制。', '',
             'PCM 源 peak=8192；mono/左声道每秒前250ms为997Hz，右声道500..750ms为613Hz，边沿5ms渐变。',
             '硬件音量由播放器保持当前配置，本工具不访问或修改音量。', '',
             '横屏 MP4 是1280×720、30fps、BP L3.1、ref1/B0/GOP30、BT601 full；未做预旋转。',
             f'帧号000..{args.seconds * FPS - 1:03d}、运动标记及不对称四角用于观察方向和连续性。', '',
             'ADTS没有容器编辑表，AAC原始编码延迟/尾padding会保留；不要把源30秒当作解码精确30秒。',
             'manifest保留源、FFmpeg输出和容器有效样本数；PC验证不代表板端已播放通过。', '',
             '| 文件 | 字节 | 源frames/ch | FFmpeg解码frames/ch |', '|---|---:|---:|---:|']
    lines += [f"| {entry['file']} | {entry['bytes']} | {entry['source_frames']} | {entry['ffmpeg_decoded_frames']} |"
              for entry in report['files']]
    (output / 'README.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    progress(f"PC VERIFIED: {len(products)} files, {report['total_copy_bytes']} bytes; {output / 'manifest.json'}")


if __name__ == '__main__':
    main()
