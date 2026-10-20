#!/usr/bin/env python3
"""Generate short CABAC/profile/reference fixtures for RAM player validation.

Each MP4 has visible frame numbers, one 48 kHz AAC-LC track, BT.601 full-range
video and no B frames. Only the H.264 feature under test changes. FFmpeg
decodes every output and trace_headers checks the actual SPS/PPS, rather than
assuming that the requested libx264 options appeared in the encoded stream.
Outputs and reports stay local under build; this script does not use the board.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ('baseline-cavlc-ref1', 'baseline', 66, 0, 1, 0, 31),
    ('main-cabac-ref1', 'main', 77, 1, 1, 0, 31),
    ('main-cabac-ref3', 'main', 77, 1, 3, 0, 31),
    ('main-cabac-ref6', 'main', 77, 1, 6, 0, 40),
    ('high-cabac-ref1', 'high', 100, 1, 1, 1, 31),
    ('high-cabac-ref3', 'high', 100, 1, 3, 1, 31),
    ('high-cabac-ref6', 'high', 100, 1, 6, 1, 40),
)


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True)
    if result.returncode:
        raise RuntimeError(f'command failed: {command}\n' +
                           result.stderr.decode(errors='replace')[-12000:])
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_formats():
    spec = importlib.util.spec_from_file_location('compat_formats',
                                                ROOT / 'scripts/make-player-format-fixtures.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def frame_crcs(ffmpeg, path, count):
    """Checksums of coded full-range NV12 bytes, with no range conversion."""
    command = [ffmpeg, '-v', 'error', '-i', path, '-map', '0:v:0', '-an',
               '-vf', 'scale=in_range=full:out_range=full,format=nv12',
               '-fps_mode', 'passthrough', '-pix_fmt', 'nv12', '-f', 'rawvideo', '-']
    process = subprocess.Popen(list(map(str, command)), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    y_bytes, frame_bytes = 1280 * 720, 1280 * 720 * 3 // 2
    result = []
    try:
        for index in range(count):
            frame = process.stdout.read(frame_bytes)
            if len(frame) != frame_bytes:
                raise RuntimeError(f'{path.name}: short raw frame {index}: {len(frame)}/{frame_bytes}')
            result.append({'index': index, 'y_crc32': f'{zlib.crc32(frame[:y_bytes]):08x}',
                           'uv_crc32': f'{zlib.crc32(frame[y_bytes:]):08x}'})
        if process.stdout.read(1):
            raise RuntimeError(f'{path.name}: excess raw frames')
        stderr = process.stderr.read()
        if process.wait():
            raise RuntimeError(stderr.decode(errors='replace'))
    finally:
        process.stdout.close()
        process.stderr.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    return result


def make_suite(ffmpeg, ffprobe, output, report, formats, seconds):
    """One diagnostic stream changes SPS/PPS at seven closed IDR boundaries.

    This validates entropy/profile/reference transitions in one RAM upload;
    it does not stand in for opening seven independent files on the board.
    """
    reports = output / '_reports'
    raw = output / '_sources/compat-suite.h264'
    raw.write_bytes(b''.join((reports / (Path(entry['file']).stem + '.h264')).read_bytes()
                            for entry in report['files']))
    audio = output / '_sources/suite-48000-1ch.wav'
    formats.write_source_audio(audio, 48000, 1, seconds * len(report['files']))
    path = output / f'compat-suite-{seconds * len(report["files"])}s.mp4'
    run([ffmpeg, '-v', 'error', '-y', '-fflags', '+genpts', '-r', '30', '-f', 'h264', '-i', raw,
         '-i', audio, '-map', '0:v:0', '-map', '1:a:0', '-c:v', 'copy',
         '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '96k', '-ar', '48000', '-ac', '1',
         '-video_track_timescale', '90000', '-movflags', '+faststart', path])
    meta = json.loads(run([ffprobe, '-v', 'error', '-count_frames', '-show_streams', '-of', 'json', path]))
    video = next(item for item in meta['streams'] if item['codec_type'] == 'video')
    expected_count = seconds * 30 * len(report['files'])
    assert int(video['nb_read_frames']) == expected_count
    expected, segments = [], []
    for entry in report['files']:
        first = len(expected)
        for crc in json.loads((output / entry['nv12_crc32']).read_text()):
            expected.append(dict(crc, index=len(expected)))
        segments.append({'file': entry['file'], 'first_frame': first, 'frames': entry['video_frames'],
                         'profile': entry['profile'], 'cabac': entry['cabac'],
                         'refs': entry['reference_frames'], 'level': entry['level']})
    actual = frame_crcs(ffmpeg, path, expected_count)
    assert actual == expected, 'suite decoded NV12 differs from independently decoded segments'
    crc_path = reports / f'{path.stem}.nv12-crc32.json'
    crc_path.write_text(json.dumps(actual, indent=2) + '\n', encoding='utf-8')
    (reports / f'{path.stem}.ffprobe.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
    result = {'file': path.name, 'bytes': path.stat().st_size, 'sha256': digest(path),
              'video_frames': expected_count, 'expected_pcm_samples': seconds * len(report['files']) * 48000,
              'nv12_crc32': crc_path.relative_to(output).as_posix(), 'segments': segments,
              'segment_crc_equivalence': True,
              'purpose': 'in-band SPS/PPS switch at closed IDRs; separate-file lifecycle not claimed'}
    print(f'{path.name}: {result["bytes"]} bytes, {expected_count} decoded frames match all seven segments', flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, default=ROOT / 'tools/ffmpeg/bin/ffmpeg.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture/compat')
    parser.add_argument('--seconds', type=int, default=3, help='1..10; short enough to embed in RAM firmware')
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES],
                        help='generate only the selected case; may be repeated')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 10:
        parser.error('--seconds must be 1..10')
    ffmpeg = args.ffmpeg.resolve()
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    output = args.output.resolve()
    reports = output / '_reports'
    for directory in (output, output / '_sources', reports):
        directory.mkdir(parents=True, exist_ok=True)
    formats = load_formats()
    audio = output / '_sources/source-48000-1ch.wav'
    audio_source = formats.write_source_audio(audio, 48000, 1, args.seconds)
    source, video_source = formats.make_picture_source(ffmpeg, output, audio, args.seconds)
    video_source['audio_channels'] = 1
    report = {'schema': 1, 'seconds': args.seconds,
              'ffmpeg': run([ffmpeg, '-version']).decode().splitlines()[0],
              'source_audio': audio_source, 'source_video': video_source, 'files': [],
              'verification': 'PC encode/header/full-decode only; hardware validation recorded separately'}
    for name, profile, profile_idc, cabac, refs, transform8, level in CASES:
        if args.case and name not in args.case:
            continue
        path = output / f'compat-{name}-{args.seconds}s.mp4'
        params = (f'cabac={cabac}:ref={refs}:bframes=0:8x8dct={transform8}:'
                  'weightp=0:keyint=30:min-keyint=30:scenecut=0:open-gop=0:aud=1:repeat-headers=1')
        run([ffmpeg, '-v', 'error', '-y', '-i', source, '-map', '0:v:0', '-map', '0:a:0',
             '-map_metadata', '-1', '-vf',
             'scale=out_color_matrix=bt601:out_range=full,format=yuv420p,setsar=1',
             '-c:v', 'libx264', '-preset', 'medium', '-threads:v', '1',
             '-profile:v', profile, '-level:v', f'{level // 10}.{level % 10}',
             '-x264-params', params, '-b:v', '2M', '-maxrate', '3M', '-bufsize', '3M',
             '-color_range', 'pc', '-colorspace', 'smpte170m', '-color_primaries', 'smpte170m',
             '-color_trc', 'smpte170m', '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '96k',
             '-ar', '48000', '-ac', '1', '-movflags', '+faststart', '-fflags', '+bitexact', path])
        meta = json.loads(run([ffprobe, '-v', 'error', '-show_streams', '-show_packets',
                               '-of', 'json', path]))
        (reports / f'{path.stem}.ffprobe.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
        video = next(item for item in meta['streams'] if item['codec_type'] == 'video')
        packets = [item for item in meta['packets'] if item['codec_type'] == 'video']
        assert video['codec_name'] == 'h264' and video['has_b_frames'] == 0
        assert (video['width'], video['height']) == (1280, 720)
        assert video['level'] == level and video['avg_frame_rate'] == '30/1'
        assert len(packets) == args.seconds * 30
        assert all(packet['pts'] == packet['dts'] for packet in packets)
        assert max(int(packet['size']) for packet in packets) <= 512 * 1024
        trace = subprocess.run(list(map(str, [ffmpeg, '-v', 'info', '-i', path, '-map', '0:v:0',
                                              '-c:v', 'copy', '-bsf:v', 'trace_headers',
                                              '-f', 'null', '-'])), capture_output=True, check=True).stderr.decode(errors='replace')
        (reports / f'{path.stem}.trace-headers.log').write_text(trace, encoding='utf-8')
        fields = {'profile_idc': profile_idc, 'level_idc': level, 'max_num_ref_frames': refs,
                  'entropy_coding_mode_flag': cabac, 'frame_mbs_only_flag': 1}
        if profile_idc == 100:
            fields['transform_8x8_mode_flag'] = transform8
            fields['bit_depth_luma_minus8'] = 0
            fields['bit_depth_chroma_minus8'] = 0
        for field, expected in fields.items():
            values = [int(value) for value in re.findall(r'\b' + field + r'\s+[^\r\n]*=\s*(\d+)', trace)]
            assert values and all(value == expected for value in values), (name, field, expected, values)
        hashes = run([ffmpeg, '-v', 'error', '-i', path, '-map', '0:v:0',
                      '-fps_mode', 'passthrough', '-f', 'framemd5', '-']).decode()
        hash_rows = [line for line in hashes.splitlines() if line and not line.startswith('#')]
        assert len(hash_rows) == len(packets)
        (reports / f'{path.stem}.framemd5').write_text(hashes, encoding='utf-8')
        crcs = frame_crcs(ffmpeg, path, len(packets))
        (reports / f'{path.stem}.nv12-crc32.json').write_text(json.dumps(crcs, indent=2) + '\n', encoding='utf-8')
        run([ffmpeg, '-v', 'error', '-y', '-i', path, '-map', '0:v:0', '-c:v', 'copy',
             '-bsf:v', 'h264_mp4toannexb', '-f', 'h264', reports / f'{path.stem}.h264'])
        entry = {'file': path.name, 'bytes': path.stat().st_size, 'sha256': digest(path),
                 'video_frames': len(packets), 'profile': video['profile'], 'level': level,
                 'reference_frames': refs, 'cabac': cabac, 'transform_8x8': transform8,
                 'b_frames': 0, 'width': 1280, 'height': 720, 'fps': '30/1',
                 'matrix': 'BT.601', 'range': 'full', 'expected_pcm_samples': args.seconds * 48000,
                 'max_video_source_bytes': max(int(packet['size']) for packet in packets),
                 'software_decoded_frames': len(hash_rows), 'headers_verified': fields,
                 'nv12_crc32': f'_reports/{path.stem}.nv12-crc32.json'}
        report['files'].append(entry)
        print(f'{path.name}: {entry["bytes"]} bytes, {len(hash_rows)} decoded frames, headers verified', flush=True)
    if len(report['files']) == len(CASES):
        report['suite'] = make_suite(ffmpeg, ffprobe, output, report, formats, args.seconds)
    (output / 'manifest.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'PC verification passed: {len(report["files"])} cases; {output / "manifest.json"}', flush=True)


if __name__ == '__main__':
    main()
