#!/usr/bin/env python3
"""Build the real SDK SPS parser and verify existing geometry/level fixtures.

Run make-player-geometry-fixtures.py and make-player-level-fixtures.py first.
This entry reads their existing MP4s; it never regenerates videos or uses serial.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def boxes(data):
    position = 0
    while position < len(data):
        assert len(data) - position >= 8, 'truncated fixture box'
        size, kind = struct.unpack_from('>I4s', data, position)
        header = 8
        if size == 1:
            assert len(data) - position >= 16
            size, header = struct.unpack_from('>Q', data, position + 8)[0], 16
        if size == 0:
            size = len(data) - position
        assert header <= size <= len(data) - position, 'invalid fixture box'
        yield kind, data[position + header:position + size]
        position += size


def extract_sps(data):
    def search(data, path):
        for kind, payload in boxes(data):
            if kind != path[0]:
                continue
            if len(path) == 1:
                return payload
            found = search(payload, path[1:])
            if found is not None:
                return found
        return None

    stsd = search(data, [b'moov', b'trak', b'mdia', b'minf', b'stbl', b'stsd'])
    assert stsd is not None
    for kind, sample in boxes(stsd[8:]):
        if kind not in (b'avc1', b'avc3'):
            continue
        for kind, avcc in boxes(sample[78:]):
            if kind == b'avcC':
                assert len(avcc) >= 8 and avcc[0] == 1 and avcc[5] & 31 == 1
                size = struct.unpack_from('>H', avcc, 6)[0]
                nal = avcc[8:8 + size]
                assert len(nal) == size and nal[0] & 31 == 7
                output, zeros = bytearray(), 0
                for index, value in enumerate(nal[1:], 1):
                    if zeros >= 2 and value == 3 and index + 1 < len(nal) and nal[index + 1] <= 3:
                        zeros = 0
                        continue
                    output.append(value)
                    zeros = zeros + 1 if value == 0 else 0
                return bytes(output)
    raise AssertionError('fixture has no avcC SPS')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/player-codec-format-host')
    parser.add_argument('--fixtures', type=Path, default=ROOT / 'build/player-fixture/geometry-color')
    parser.add_argument('--levels', type=Path, default=ROOT / 'build/player-fixture/geometry-level-tags')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    derived = output / 'derived-libh264'
    load('format_prepare', ROOT / 'scripts/prepare-player-codec.py').prepare(derived)
    # Only this host copy needs a member in the vendor's unused empty struct.
    if os.name == 'nt':
        sei = derived / 'include/h264_sei.h'
        text = sei.read_text(encoding='utf-8')
        anchor = 'typedef struct H264SEIA53Caption {\n'
        assert text.count(anchor) == 1
        sei.write_text(text.replace(anchor, anchor + '    int player_host_unused;\n'), encoding='utf-8')
    player = ROOT / 'projects/halley7m-player'
    sources = [ROOT / 'tests/player_codec_format_test.c', player / 'common/player_video.c']
    sources += [derived / 'src' / name for name in ('h264_ps.c', 'golomb.c', 'h264data.c', 'mathtables.c')]
    includes = [player / 'codec', player / 'common', derived, derived / 'include']
    executable = output / ('player_codec_format_test.exe' if os.name == 'nt' else 'player_codec_format_test')
    environment = (load('format_host_env', ROOT / 'scripts/test-player-amp.py').msvc_environment()
                   if os.name == 'nt' else os.environ.copy())
    compiler = shutil.which('cl' if os.name == 'nt' else environment.get('CC', 'cc'),
                           path=environment.get('Path', environment.get('PATH')))
    if not compiler:
        raise RuntimeError('Host C compiler not found')
    if os.name == 'nt':
        command = [compiler, '/nologo', '/std:c11', '/O2', '/utf-8', '/UNDEBUG', '/W3',
                   '/wd4090', '/wd4113', '/wd4028', '/wd4005']
        command += ['/I' + str(path) for path in includes] + list(map(str, sources)) + ['/Fe:' + str(executable)]
    else:
        command = [compiler, '-std=gnu11', '-O2', '-UNDEBUG', '-w']
        command += [part for path in includes for part in ('-I', str(path))]
        command += list(map(str, sources)) + ['-lm', '-o', str(executable)]
    build = subprocess.run(command, cwd=output, env=environment, capture_output=True)
    (output / 'build.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        raise RuntimeError('SPS host build failed; see ' + str(output / 'build.log'))

    keys = ('coded_width', 'coded_height', 'visible_width', 'visible_height', 'crop_left', 'crop_right',
            'crop_top', 'crop_bottom', 'sar_num', 'sar_den', 'matrix_coefficients', 'full_range',
            'profile_idc', 'level_idc')
    inputs, expected = [], []
    # Geometry manifest ends with SAR16:15 + explicit601limited; keep that
    # record immediately before the absent-VUI replacement of SPS id0.
    for directory in (args.levels.resolve(), args.fixtures.resolve()):
        manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        for item in manifest['files']:
            data = (directory / item['file']).read_bytes()
            assert hashlib.sha256(data).hexdigest() == item['sha256'], item['file']
            target = output / (Path(item['file']).stem + '.spsrbsp')
            target.write_bytes(extract_sps(data))
            inputs.append(target)
            expected.append({key: item[key] for key in keys})
    # The final id0 has explicit SAR and colours. Clear its VUI to check that
    # the actual parser drops stale fields when that same SPS id is replaced.
    trace = (args.fixtures / '_reports' / (inputs[-1].stem + '.trace-headers.log')).read_text(encoding='utf-8')
    match = re.search(r'\]\s+(\d+)\s+vui_parameters_present_flag', trace)
    assert match, 'missing SPS VUI bit position in fixture trace'
    position = int(match.group(1)) - 8  # trace includes the one-byte NAL header.
    bits = ''.join(f'{value:08b}' for value in inputs[-1].read_bytes())[:position] + '01'
    bits += '0' * (-len(bits) % 8)
    omitted = output / 'reuse-id0-no-vui.spsrbsp'
    omitted.write_bytes(bytes(int(bits[index:index + 8], 2) for index in range(0, len(bits), 8)))
    inputs.append(omitted)
    absent = dict(expected[-1], sar_num=0, sar_den=0, matrix_coefficients=2, full_range=-1)
    expected.append(absent)
    tested = subprocess.run([str(executable), *map(str, inputs)], capture_output=True)
    (output / 'test.log').write_bytes(tested.stdout + tested.stderr)
    if tested.returncode:
        raise RuntimeError('SPS host test failed; see ' + str(output / 'test.log'))
    rows = [json.loads(line) for line in tested.stdout.decode().splitlines() if line.startswith('{')]
    assert len(rows) == len(expected), (len(rows), len(expected))
    results = []
    for path, actual, reference in zip(inputs, rows, expected):
        assert {key: actual[key] for key in reference} == reference, (path.name, actual, reference)
        if path == omitted:
            assert actual['video_signal'] == actual['colour_description'] == 0
            assert actual['colour_primaries'] == actual['transfer_characteristics'] == 2
        else:
            assert actual['video_signal'] == actual['colour_description'] == 1
            assert actual['colour_primaries'] == actual['transfer_characteristics'] == reference['matrix_coefficients']
        results.append(dict(file=path.name, metadata=actual))
        print('PASS actual SPS parser/helper: ' + path.name)
    (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(f'PASS {len(rows) - 1} existing MP4 SPS records plus reused id0 without VUI; logs: {output}')


if __name__ == '__main__':
    main()
