"""Retag existing stage4 720p fixtures without transcoding VCL or production edits."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/player-fixture/geometry-level-tags'
FFMPEG = ROOT / 'tools/ffmpeg/bin/ffmpeg.exe'
FFPROBE = FFMPEG.with_name('ffprobe.exe')


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def nals(data):
    marks = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    return [data[mark.end():marks[index + 1].start() if index + 1 < len(marks) else len(data)]
            for index, mark in enumerate(marks)]


def main():
    global OUT, FFMPEG, FFPROBE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--fixtures', type=Path, default=ROOT / 'build/player-fixture/geometry-color')
    parser.add_argument('--ffmpeg', type=Path, default=FFMPEG)
    args = parser.parse_args()
    OUT = args.output.resolve()
    FFMPEG = args.ffmpeg.resolve()
    FFPROBE = FFMPEG.with_name('ffprobe' + FFMPEG.suffix)
    reports = OUT / '_reports'
    reports.mkdir(parents=True, exist_ok=True)
    native = OUT / '_host'
    native.mkdir(exist_ok=True)
    environment = load('amp_environment', ROOT / 'scripts/test-player-amp.py').msvc_environment()
    compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH')))
    common = ROOT / 'projects/halley7m-player/common'
    executable = native / 'player_mp4_test.exe'
    run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8', '/I' + str(common),
         ROOT / 'tests/player_mp4_test.c', common / 'player_mp4.c', '/Fe:' + str(executable)],
        cwd=native, env=environment)
    geometry = load('geometry_fixture_helper', ROOT / 'scripts/make-player-geometry-fixtures.py')
    base_dir = args.fixtures.resolve()
    base = base_dir / 'geometry-1280x720-601-full.mp4'
    baseline = json.loads((base_dir / 'manifest.json').read_text(encoding='utf-8'))
    reference = next(item for item in baseline['files'] if item['file'] == base.name)
    source_prefix = reports / 'source-level31'
    run([executable, base, source_prefix, 'short-read'])
    base_nals = nals(Path(str(source_prefix) + '.h264').read_bytes())
    base_vcl = [nal for nal in base_nals if nal[0] & 31 in (1, 5)]
    report = {'schema': 1, 'status': 'building', 'source': str(base), 'source_sha256': sha(base),
              'scope': 'PC-only level retag; unchanged coded VCL and decoded pixels/AAC; no board claim',
              'files': []}
    for level, number in [('4.1', 41), ('4.2', 42)]:
        name = 'level-' + level + '-1280x720-601-full'
        path = OUT / (name + '.mp4')
        run([FFMPEG, '-v', 'error', '-y', '-i', base, '-map', '0:v:0', '-map', '0:a:0', '-c', 'copy',
             '-bsf:v', 'h264_metadata=level=' + level, '-map_metadata', '-1', '-movflags', '+faststart', path])
        meta = json.loads(run([FFPROBE, '-v', 'error', '-show_streams', '-show_packets', '-of', 'json', path]))
        save(reports / (name + '.ffprobe.json'), meta)
        video = next(stream for stream in meta['streams'] if stream['codec_type'] == 'video')
        audio = next(stream for stream in meta['streams'] if stream['codec_type'] == 'audio')
        assert video['level'] == number and video['width'] == 1280 and video['height'] == 720
        assert video['nb_frames'] == '90' and video['profile'] == 'Constrained Baseline'
        assert video['has_b_frames'] == 0 and video['avg_frame_rate'] == '30/1'
        assert int(audio['duration_ts']) == 144000 and int(audio['sample_rate']) == 48000
        assert audio['channels'] == 1
        result = subprocess.run(list(map(str, [FFMPEG, '-v', 'info', '-i', path, '-map', '0:v:0',
                                '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'])),
                                capture_output=True, check=True)
        trace = result.stderr.decode(errors='replace')
        (reports / (name + '.trace-headers.log')).write_text(trace, encoding='utf-8')
        values = [int(value) for value in re.findall(r'\blevel_idc\s+[^\r\n]*=\s*(\d+)', trace)]
        assert values and set(values) == {number}, values
        prefix = reports / name
        rows = [json.loads(line) for line in run([executable, path, prefix, 'short-read']).decode().splitlines()]
        assert rows[0]['info']['video']['level'] == number and rows[-1]['done']['video'] == 90
        retagged = nals(Path(str(prefix) + '.h264').read_bytes())
        vcl = [nal for nal in retagged if nal[0] & 31 in (1, 5)]
        assert vcl == base_vcl
        non_sps = lambda sequence: [nal for nal in sequence if nal[0] & 31 != 7]
        assert non_sps(retagged) == non_sps(base_nals)
        for suffix in ('.aacraw', '.asc'):
            assert Path(str(prefix) + suffix).read_bytes() == Path(str(source_prefix) + suffix).read_bytes()
        crc = geometry.nv12_crcs(FFMPEG, path, reports, 1280, 720, True, False)
        assert crc['visible_nv12_sha256'] == reference['visible_nv12_sha256']
        assert crc['frame000'] == reference['frame000'] and crc['frame089'] == reference['frame089']
        item = {**reference, **crc, 'file': path.name, 'bytes': path.stat().st_size, 'sha256': sha(path),
                'level_idc': number, 'reference_file': base.name, 'vcl_nals': len(vcl), 'vcl_payload_identical': True,
                'non_sps_nals_identical': True, 'aac_au_and_asc_identical': True}
        report['files'].append(item)
        save(OUT / 'manifest.json', report)
        print(f'PASS {path.name}: {item["bytes"]}B, level={number}, VCL/AAC exact, 90-frame NV12 exact', flush=True)
    report['status'] = 'pc-verified'
    report['total_copy_bytes'] = sum(row['bytes'] for row in report['files'])
    save(OUT / 'manifest.json', report)
    (OUT / 'README.md').write_text(
        '# Level 声明诊断样片\n\n'
        '顶层两个MP4仅把既有1280x720、BP、B0、ref1、601full样片的SPS/avcC level改为4.1和4.2。\n'
        'VCL NAL、其他非SPS NAL、AAC AU和ASC逐字节相同；90帧PC NV12与level3.1原片完整SHA/首尾CRC一致。\n'
        '每个3秒、90帧、144000个有效音频sample。只复制顶层MP4，_reports是PC证据。\n\n'
        '这不能证明1080p、4.1/4.2上限吞吐或DPB能力。\n', encoding='utf-8')
    print('PC verified2 fixtures; total ' + str(report['total_copy_bytes']) + 'B', flush=True)


if __name__ == '__main__':
    main()
