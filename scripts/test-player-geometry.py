"""Targeted real-source/session format tests; fake VPU/DPU, no board or serial."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'scripts/test-player-amp.py').is_file())


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, **kwargs)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout.decode(errors='replace')[-12000:],
                              result.stderr.decode(errors='replace')[-12000:]))
    return result.stdout


def format_values(item):
    return [item['coded_width'], item['coded_height'], item['visible_width'], item['visible_height'],
            item['crop_left'], item['crop_top'], item['sar_num'], item['sar_den'],
            item['matrix_coefficients'], item['full_range'], item['matrix_coefficients'], item['matrix_coefficients']]


def change_entry(data, helper, *, width=None, height=None, colour=None, sar=None, append_colour=False):
    def change(kind, payload):
        if kind != b'stsd':
            return kind, payload
        entries = list(helper.boxes(payload[8:]))
        assert len(entries) == 1
        entry, sample = entries[0]
        if entry != b'avc1':
            return kind, payload
        header = bytearray(sample[:78])
        if width is not None:
            struct.pack_into('>HH', header, 24, width, height)
        children, found_colour, found_sar = bytearray(), False, False
        for tag, body in helper.boxes(sample[78:]):
            if tag == b'colr' and colour is not None:
                assert len(body) == len(colour)
                body, found_colour = colour, True
            if tag == b'pasp' and sar is not None:
                assert len(body) == 8
                body, found_sar = struct.pack('>II', *sar), True
            children += helper.box(tag, body)
        if append_colour and not found_colour:
            assert colour is not None
            # Only a moov-after-mdat file may grow without stco relocation.
            top = [tag for tag, _ in helper.boxes(data)]
            assert top.index(b'mdat') < top.index(b'moov')
            children += helper.box(b'colr', colour)
        else:
            assert colour is None or found_colour
        assert sar is None or found_sar
        return kind, payload[:8] + helper.box(entry, bytes(header) + children)
    return helper.rewrite(data, change)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--player-root', type=Path, default=ROOT / 'projects/halley7m-player')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/player-host/geometry')
    parser.add_argument('--fixtures', type=Path, default=ROOT / 'build/player-fixture/geometry-color')
    parser.add_argument('--levels', type=Path, default=ROOT / 'build/player-fixture/geometry-level-tags')
    args = parser.parse_args()
    player, output = args.player_root.resolve(), args.build.resolve()
    ffmpeg = ROOT / 'tools/ffmpeg/bin/ffmpeg.exe'
    support = load('geometry_session_support', player / 'mips/test-session.py')
    support.ROOT, support.SOURCE = ROOT, player / 'mips'
    output, executables, base, libs, environment = support.build_host(output)
    run([executables[0]])
    source_exe = output / ('player_source_test.exe' if os.name == 'nt' else 'player_source_test')
    source_test = player.parents[1] / 'tests/player_source_test.c'
    common = player / 'common'
    command = base + (['/D_CRT_SECURE_NO_WARNINGS'] if os.name == 'nt' else [])
    command += [source_test, common / 'player_source.c', common / 'player_media.c', common / 'player_mp4.c', *libs]
    command += ['/Fe:' + str(source_exe)] if os.name == 'nt' else ['-lm', '-o', source_exe]
    run(command, cwd=output, env=environment)
    results = []

    def source(path):
        prefix = output / ('source-' + path.stem)
        rows = [json.loads(line) for line in run([source_exe, path, prefix, 'short-read']).decode().splitlines()]
        return rows, next(row['info'] for row in rows if 'info' in row), rows[-1]['done']

    def session(path, metadata, *, mode=None, depth=0):
        rows, info, done = source(path)
        video = [row['packet'] for row in rows if row.get('packet', {}).get('kind') == 1]
        expected = max(done['output_frames'], (info['video_end_us'] * 48000 + 999999) // 1000000)
        last = max(video, key=lambda packet: packet['pts_us'])['index']
        env = environment.copy()
        env['PLAYER_TEST_VIDEO_FORMAT'] = ','.join(map(str, metadata))
        env['PLAYER_TEST_FORMAT_BUSY'] = '5'
        if mode:
            env.update(mode)
        run([executables[1], path, len(video), expected, depth, last,
             info['video_display_frames'], expected - done['output_frames']], env=env)
        results.append({'file': path.name, 'frames': len(video), 'source_pcm_frames': done['output_frames'],
                        'session_samples': expected, 'format': metadata, 'mode': mode or 'three pause/resume/EOF/STOP runs'})
        print('PASS real source + session coded allocation/READY-format/audio gate: ' + path.name, flush=True)
        return info, done

    manifest = json.loads((args.fixtures / 'manifest.json').read_text(encoding='utf-8'))
    for item in manifest['files']:
        info, done = session(args.fixtures / item['file'], format_values(item))
        assert info['video_frames'] == 90 and done['video'] == 90 and done['native_frames'] == 144000
    levels = json.loads((args.levels / 'manifest.json').read_text(encoding='utf-8'))
    for item in levels['files']:
        session(args.levels / item['file'], format_values(item))
    helper = load('geometry_mp4_helpers', ROOT / 'scripts/test-player-mp4.py')
    native = next(item for item in manifest['files'] if item['file'] == 'geometry-1280x720-601-full.mp4')
    original = (args.fixtures / native['file']).read_bytes()
    # Same real SDR SPS; a contradictory container cannot override it.
    hdr_colr = b'nclx' + struct.pack('>HHHB', 9, 16, 9, 0)
    contradictory = output / 'sps-sdr-wins-container-hdr.mp4'
    tail = output / 'known-sps-moov-tail.mp4'
    run([ffmpeg, '-v', 'error', '-y', '-i', args.fixtures / native['file'], '-map', '0', '-c', 'copy', tail])
    contradictory.write_bytes(change_entry(tail.read_bytes(), helper, colour=hdr_colr, append_colour=True))
    session(contradictory, format_values(native))
    sar_item = next(item for item in manifest['files'] if item['sar_num'] == 16)
    sar_changed = output / 'sps-landscape-sar-wins-container-portrait.mp4'
    sar_changed.write_bytes(change_entry((args.fixtures / sar_item['file']).read_bytes(), helper, sar=(1, 4)))
    session(sar_changed, format_values(sar_item))
    session(sar_changed, format_values(sar_item), mode={'PLAYER_TEST_STOP_FORMAT': '1'})

    def reject_source(name, width, height):
        path = output / name
        path.write_bytes(change_entry(original, helper, width=width, height=height))
        result = subprocess.run(list(map(str, [source_exe, path, output / 'invalid'])), capture_output=True)
        assert result.returncode == 3 and json.loads(result.stdout.decode().splitlines()[-1])['error'] == -3
        env = environment.copy()
        env['PLAYER_TEST_EXPECT_ERROR'] = '-3'
        run([executables[1], path, '90', '144000'], env=env)
        results.append({'file': name, 'result': 'source/session reject before audio/allocation'})
        print('PASS source/session dimension rejection: ' + name, flush=True)
    reject_source('oversized-1920x1080.mp4', 1920, 1080)
    reject_source('odd-853x480.mp4', 853, 480)
    reject_source('coded-padding-over-budget-2032x450.mp4', 2032, 450)
    unknown = output / 'no-sps-colour.mp4'
    run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=blue:s=1280x720:r=30:d=0.4',
         '-f', 'lavfi', '-i', 'sine=f=997:r=48000:d=0.4', '-c:v', 'libx264', '-profile:v', 'baseline',
         '-level:v', '3.1', '-refs', '1', '-bf', '0', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-ac', '1', unknown])
    trace = subprocess.run(list(map(str, [ffmpeg, '-v', 'verbose', '-i', unknown, '-map', '0:v:0',
                           '-c:v', 'copy', '-bsf:v', 'trace_headers', '-f', 'null', '-'])), capture_output=True, check=True)
    flags = [line.rsplit('=', 1)[-1].strip() for line in trace.stderr.decode(errors='replace').splitlines()
             if 'video_signal_type_present_flag' in line]
    assert flags and set(flags) == {'0'}
    hdr = output / 'container-hdr-without-sps-colour.mp4'
    hdr.write_bytes(change_entry(unknown.read_bytes(), helper, colour=hdr_colr, append_colour=True))
    unknown_format = [1280, 720, 1280, 720, 0, 0, 0, 0, 2, -1, 2, 2]
    session(hdr, unknown_format, mode={'PLAYER_TEST_EXPECT_ERROR': 'format'})
    session(unknown, unknown_format, mode={'PLAYER_TEST_FORMAT_CHANGE': '1', 'PLAYER_TEST_EXPECT_ERROR': 'format'})
    # A B-frame source with no video edit has a genuine positive first PTS.
    bframes = output / 'positive-pts-source.mp4'
    run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=s=1280x720:r=30:d=1.2',
         '-f', 'lavfi', '-i', 'sine=f=500:r=48000:d=1.2', '-c:v', 'libx264', '-profile:v', 'main',
         '-level:v', '3.1', '-refs', '3', '-bf', '2', '-pix_fmt', 'yuv420p', '-video_track_timescale', '90000',
         '-movie_timescale', '1000', '-x264-params', 'b-adapt=0:scenecut=0', '-c:a', 'aac', bframes])
    delayed = output / 'positive-first-pts-format-wait.mp4'
    changed, _ = support.video_edit(bframes.read_bytes(), helper, remove=True)
    delayed.write_bytes(changed)
    rows, _, _ = source(delayed)
    assert min(row['packet']['pts_us'] for row in rows if row.get('packet', {}).get('kind') == 1) > 0
    session(delayed, unknown_format, depth=2)
    (output / 'geometry-wiring-results.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    print(f'PASS {len(results)} targeted real-file source/session cases; fake codec/DPU only, no board proof', flush=True)


if __name__ == '__main__':
    main()
