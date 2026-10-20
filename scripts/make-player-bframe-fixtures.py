#!/usr/bin/env python3
"""Generate short B-frame MP4s and presentation-order NV12/PTS references."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ('main-ref3-b2', 'main', 3, 2, 'none', 90000),
    ('high-ref6-b4', 'high', 6, 4, 'normal', 90000),
    ('high-ref6-b4-ms', 'high', 6, 4, 'normal', 1000),
)

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path, default=ROOT / 'tools/ffmpeg/bin/ffmpeg.exe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture/bframes')
    args = parser.parse_args()
    ffmpeg = args.ffmpeg.resolve()
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    output = args.output.resolve()
    for directory in (output, output / '_sources', output / '_reports'):
        directory.mkdir(parents=True, exist_ok=True)
    formats = load('bframe_formats', ROOT / 'scripts/make-player-format-fixtures.py')
    compat = load('bframe_compat', ROOT / 'scripts/make-player-compat-fixtures.py')
    audio = output / '_sources/source-48000-1ch-4s.wav'
    formats.write_source_audio(audio, 48000, 1, 4)
    source, _ = formats.make_picture_source(ffmpeg, output, audio, 4)
    frames, samples = 99, 99 * 1600
    report = {'schema': 1, 'video_frames': frames, 'expected_pcm_samples': samples,
              'note': 'PC decode/PTS checks only; hardware results recorded separately', 'files': []}
    for name, profile, refs, bframes, pyramid, timescale in CASES:
        path = output / f'bframe-{name}-{frames}frames.mp4'
        params = (f'cabac=1:ref={refs}:bframes={bframes}:b-adapt=0:b-pyramid={pyramid}:'
                  f'8x8dct={1 if profile == "high" else 0}:weightp=0:weightb=0:'
                  'keyint=30:min-keyint=30:scenecut=0:open-gop=0:aud=1:repeat-headers=1')
        compat.run([ffmpeg, '-v', 'error', '-y', '-i', source, '-map', '0:v:0', '-map', '0:a:0',
                    '-vf', 'scale=out_color_matrix=bt601:out_range=full,format=yuv420p,setsar=1',
                    '-c:v', 'libx264', '-preset', 'medium', '-threads:v', '1',
                    '-profile:v', profile, '-level:v', '4.0', '-x264-params', params,
                    '-b:v', '2M', '-maxrate', '3M', '-bufsize', '3M', '-frames:v', str(frames),
                    '-color_range', 'pc', '-colorspace', 'smpte170m',
                    '-af', f'atrim=end_sample={samples},asetpts=PTS-STARTPTS',
                    '-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '96k', '-ar', '48000', '-ac', '1',
                    '-movie_timescale', '48000', '-video_track_timescale', str(timescale),
                    '-movflags', '+faststart', path])
        meta = json.loads(compat.run([ffprobe, '-v', 'error', '-show_streams', '-show_packets', '-of', 'json', path]))
        video = next(item for item in meta['streams'] if item['codec_type'] == 'video')
        audio_meta = next(item for item in meta['streams'] if item['codec_type'] == 'audio')
        packets = [item for item in meta['packets'] if item['codec_type'] == 'video']
        assert len(packets) == frames and video['has_b_frames'] > 0
        assert int(audio_meta['duration_ts']) == samples, audio_meta
        ordered = sorted(enumerate(packets), key=lambda item: int(item[1]['pts']))
        assert [item[0] for item in ordered] != list(range(frames))
        crcs = compat.frame_crcs(ffmpeg, path, frames)
        for index, (decode_index, packet) in enumerate(ordered):
            crcs[index]['output_index'] = index
            crcs[index]['index'] = decode_index
            crcs[index]['pts_us'] = int(packet['pts']) * 1000000 // timescale
            crcs[index]['duration_us'] = int(packet['duration']) * 1000000 // timescale
        assert crcs[0]['pts_us'] == 0 and crcs[-1]['index'] != frames - 1
        prefix = output / '_reports' / path.stem
        Path(str(prefix) + '.nv12-crc32.json').write_text(json.dumps(crcs, indent=2) + '\n', encoding='utf-8')
        Path(str(prefix) + '.ffprobe.json').write_text(json.dumps(meta, indent=2) + '\n', encoding='utf-8')
        trace = subprocess.run(list(map(str, [ffmpeg, '-v', 'info', '-i', path, '-map', '0:v:0',
                                        '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'])),
                                        capture_output=True, check=True).stderr.decode(errors='replace')
        Path(str(prefix) + '.trace-headers.log').write_text(trace, encoding='utf-8')
        def field(name):
            values = {int(value) for value in re.findall(r'\b' + name + r'\s+[^\r\n]*=\s*(\d+)', trace)}
            assert len(values) == 1, (name, values)
            return values.pop()
        assert field('max_num_ref_frames') == refs
        report['files'].append({'file': path.name, 'bytes': path.stat().st_size, 'sha256': compat.digest(path),
                                'profile': profile, 'refs': refs, 'bframes_setting': bframes,
                                'reorder_frames': field('max_num_reorder_frames'),
                                'max_dec_frame_buffering': field('max_dec_frame_buffering'),
                                'video_frames': frames, 'expected_pcm_samples': samples,
                                'timescale': timescale, 'last_output_decode_index': crcs[-1]['index'],
                                'nv12_crc32': prefix.relative_to(output).as_posix() + '.nv12-crc32.json'})
        print(path.name, report['files'][-1], flush=True)
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

if __name__ == '__main__':
    main()
