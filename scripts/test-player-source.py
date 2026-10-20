"""Test the production unified source against real decoders and FFmpeg packets.

No board, serial port, SDK mutation, or hardware audio is involved.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from fractions import Fraction

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg', type=Path)
    parser.add_argument('--probe-media', type=Path, help='Also verify a user MP4 through the production source')
    parser.add_argument('--mips-prefix', help="Also compile production source.c using the real compiler prefix ending in '-'")
    args = parser.parse_args()
    helper = load('test-player-audio-decode')
    helper.verify_sources()
    output = ROOT / 'build/player-host/source'
    output.mkdir(parents=True, exist_ok=True)
    environment = load('test-player-amp').msvc_environment() if os.name == 'nt' else os.environ.copy()
    decode_build = output / 'decode'
    configure = ['cmake', '-S', ROOT / 'projects/halley7m-player/audio-decode', '-B', decode_build,
                 '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release']
    if os.name == 'nt':
        configure.append('-DCMAKE_C_FLAGS_DEBUG=/D_DEBUG /MTd /Ob0 /Od /RTC1 /Z7')
    helper.run(configure, env=environment)
    helper.run(['cmake', '--build', decode_build, '--parallel', '8'], env=environment)
    common = ROOT / 'projects/halley7m-player/common'
    decode = ROOT / 'projects/halley7m-player/audio-decode'
    sources = [ROOT / 'tests/player_source_test.c', common / 'player_source.c',
               common / 'player_media.c', common / 'player_mp4.c']
    executable = output / ('player_source_test.exe' if os.name == 'nt' else 'player_source_test')
    if os.name == 'nt':
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH')))
        command = [compiler, '/nologo', '/std:c11', '/O2', '/MD', '/W4', '/WX', '/UNDEBUG', '/utf-8',
                   '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(common), '/I' + str(decode)]
        command += sources + [decode_build / 'player_audio_decode.lib', decode_build / 'player_faad_fixed.lib',
                              '/Fe:' + str(executable)]
    else:
        command = [os.environ.get('CC', 'gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-UNDEBUG', '-I', common, '-I', decode] + sources
        command += [decode_build / 'libplayer_audio_decode.a', decode_build / 'libplayer_faad_fixed.a', '-lm', '-o', executable]
    helper.run(command, cwd=output, env=environment)
    if args.mips_prefix:
        suffix = '.exe' if os.name == 'nt' else ''
        helper.run([args.mips_prefix + 'gcc' + suffix, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-mips32r2', '-mabi=32', '-mfp32', '-mhard-float', '-EL', '-mno-abicalls', '-fno-pic',
                    '-G0', '-fstack-usage', '-I', common, '-c', common / 'player_source.c',
                    '-o', output / 'player_source.mips.o'])
        print('PASS real MIPS GNU11/O2 source adapter compilation', flush=True)
    print(helper.run([executable]).decode().strip(), flush=True)
    ffmpeg = args.ffmpeg or Path(json.loads((ROOT / 'build/player-tools/dependencies.json').read_text())['ffmpeg']['executable'])
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    reports = []

    def check(path, expected_frames=None, video_trim=False):
        prefix = output / ('out-' + path.stem)
        def artifact(suffix):
            return Path(str(prefix) + suffix)
        rows = [json.loads(line) for line in helper.run([executable, path, prefix, 'short-read']).decode().splitlines()]
        info, done = rows[1]['info'], rows[-1]['done']
        meta = json.loads(helper.run([ffprobe, '-v', 'error', '-show_packets', '-show_streams', '-show_data', '-of', 'json', path]))
        audio_index = next(stream['index'] for stream in meta['streams'] if stream['codec_type'] == 'audio')
        reference_packets = [p for p in meta['packets'] if p['stream_index'] == audio_index]
        packets = [row['packet'] for row in rows if 'packet' in row and row['packet']['kind'] == 2]
        assert len(packets) == len(reference_packets) == done['packets'], path
        data = artifact('.packets').read_bytes()
        count, = struct.unpack_from('<I', data)
        cursor = 4
        assert count == len(packets)
        for packet, reference in zip(packets, reference_packets):
            length, = struct.unpack_from('<I', data, cursor)
            cursor += 4
            expected = helper.dump_bytes(reference['data'])
            assert length == packet['bytes'] == len(expected)
            assert packet['offset'] == int(reference['pos'])
            assert data[cursor:cursor + length] == expected, (path, packet)
            cursor += length
        assert cursor == len(data)
        if info['kind'] == 5:
            # Compare the production adapter, not just the MP4 demux. Real
            # container timestamps must survive queuing without an index/30
            # reconstruction. AAC preroll remains signed after its edit.
            by_kind = {1: [], 2: []}
            for stream in meta['streams']:
                kind = 1 if stream['codec_type'] == 'video' else 2
                scale = Fraction(stream['time_base'])
                by_kind[kind] = [(packet, scale) for packet in meta['packets']
                                 if packet['stream_index'] == stream['index']]
            for kind in (1, 2):
                actual = [row['packet'] for row in rows if 'packet' in row and row['packet']['kind'] == kind]
                if kind == 1 and video_trim:
                    # FFmpeg stops demux at an edited tail; this player still
                    # decodes those coded pictures to preserve references.
                    # All packets FFmpeg exposes must still match exactly.
                    actual_by_offset = {packet['offset']: packet for packet in actual}
                    assert len(actual) >= len(by_kind[kind])
                    paired = [(actual_by_offset[int(reference['pos'])], (reference, scale))
                              for reference, scale in by_kind[kind]]
                else:
                    assert len(actual) == len(by_kind[kind])
                    paired = zip(actual, by_kind[kind])
                dts_bias = None
                for packet, (reference, scale) in paired:
                    for key in ('pts', 'duration'):
                        assert abs(Fraction(packet[key + '_us'], 1000000) - int(reference[key]) * scale) < Fraction(2, 1000000), (path, packet, reference)
                    # Signed CTTS remuxes can shift FFmpeg's decode timeline;
                    # presentation stays exact and the DTS bias stays fixed.
                    bias = Fraction(packet['dts_us'], 1000000) - int(reference['dts']) * scale
                    if dts_bias is None:
                        dts_bias = bias
                    assert abs(bias - dts_bias) < Fraction(2, 1000000), (path, packet, reference)
            clips = [row['video_clip'] for row in rows if 'video_clip' in row]
            video_packets = [row['packet'] for row in rows if 'packet' in row and row['packet']['kind'] == 1]
            visible = []
            for packet, clip in zip(video_packets, clips):
                start = max(packet['pts_us'], info['video_start_us'])
                end = min(packet['pts_us'] + packet['duration_us'], info['video_end_us'])
                assert clip['visible'] == int(end > start)
                if clip['visible']:
                    assert clip['pts_us'] == start and clip['duration_us'] == end - start
                    visible.append(packet)
            assert len(visible) == info['video_display_frames']
            if visible:
                assert max(packet['pts_us'] for packet in visible) == info['video_last_pts_us']
        skipped = int(next((item['skip_samples'] for item in reference_packets[0].get('side_data_list', [])
                            if 'skip_samples' in item), 0))
        reference = helper.run([ffmpeg, '-v', 'error', '-i', path, '-map', '0:a:0', '-f', 's16le', '-acodec', 'pcm_s16le', '-'])
        effective_start = max(info['clip_start'], done['decoder_skipped_frames'])
        reference_offset = effective_start - skipped
        assert reference_offset >= 0
        width = 2 * info['channels']
        reference = reference[reference_offset * width:(reference_offset + done['native_frames']) * width]
        error = helper.compare(artifact('.native.s16').read_bytes(), reference,
                               info['channels'], done['native_frames'], path.name)
        assert done['output_frames'] == done['native_frames'] * 48000 // info['rate']
        assert len(artifact('.mono.s16').read_bytes()) == 2 * done['output_frames']
        if expected_frames is not None:
            assert done['native_frames'] == expected_frames, (path, done, expected_frames)
        if done['video']:
            decoded = helper.run([ffmpeg, '-v', 'error', '-i', artifact('.h264'), '-f', 'framemd5', '-'])
            assert sum(1 for line in decoded.splitlines() if not line.startswith(b'#')) == done['video']
        reports.append({'name': path.name, **info, **done, **error})
        print(f"PASS {path.name}: {done['packets']} exact AU, native={done['native_frames']} normalized={done['output_frames']}", flush=True)
        return done

    def reject(path):
        result = subprocess.run([str(executable), str(path), str(output / 'invalid')], capture_output=True)
        assert result.returncode in (3, 4), (path, result.returncode, result.stdout, result.stderr)

    for rate in (44100, 48000):
        for channels in (1, 2):
            count = rate // 3 + 17
            raw = output / f'{rate}-{channels}.s16'
            # Reuse exactly the deterministic fixture synthesis, but do not
            # rely on another test having been run first.
            import array
            import math
            pcm = array.array('h', (round(6000 * math.sin(2 * math.pi * (317 + ch * 137) * n / rate) +
                                         1400 * math.sin(2 * math.pi * 1703 * n / rate))
                                   for n in range(count) for ch in range(channels)))
            raw.write_bytes(pcm.tobytes())
            base = [ffmpeg, '-v', 'error', '-y', '-f', 's16le', '-ar', rate, '-ac', channels, '-i', raw]
            media = []
            for suffix, options, expected in (
                ('gapless.mp3', ['-c:a', 'libmp3lame', '-b:a', '128k'], count),
                ('bare.mp3', ['-c:a', 'libmp3lame', '-b:a', '128k', '-write_xing', '0', '-id3v2_version', '0'], None),
                ('aac', ['-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '128k', '-f', 'adts'], None),
                ('m4a', ['-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '128k'], count),
                ('ms.m4a', ['-c:a', 'aac', '-profile:a', 'aac_low', '-b:a', '128k', '-movie_timescale', '1000'], None)):
                path = output / f'{rate}-{channels}.{suffix}'
                helper.run(base + options + [path])
                check(path, expected)
                media.append(path)
            if rate == 48000 and channels == 1:
                # Upward rounding within a single movie tick was accepted;
                # a genuinely out-of-track edit still fails.
                mp4_helper = load('test-player-mp4')
                def overlong_edit(tag, payload):
                    if tag == b'elst':
                        assert payload[0] == 0
                        payload = payload[:8] + struct.pack('>I', 10000) + payload[12:]
                    return tag, payload
                invalid = output / 'invalid-edit.m4a'
                invalid.write_bytes(mp4_helper.rewrite(media[-1].read_bytes(), overlong_edit))
                reject(invalid)
                original = media[3].read_bytes()
                asc = Path(str(output / ('out-' + media[3].stem)) + '.asc').read_bytes()
                assert len(asc) == 5
                esds = original.index(b'esds')
                box_end = esds - 4 + int.from_bytes(original[esds - 4:esds], 'big')
                asc_offset = original.index(asc, esds + 4, box_end)
                zero_rate = int('00010' + '1111' + '0' * 24 + '0001' + '000', 2).to_bytes(5, 'big')
                invalid = output / 'zero-explicit-rate.m4a'
                invalid.write_bytes(original[:asc_offset] + zero_rate + original[asc_offset + 5:])
                reject(invalid)
            # Complete-looking container/header with a truncated final payload
            # must never produce a successful EOF.
            for path in media[:4]:
                truncated = output / ('truncated-' + path.name)
                original = path.read_bytes()
                if path.suffix == '.m4a':
                    truncated.write_bytes(original[:-17])
                else:
                    truncated.write_bytes(original[:-1])
                reject(truncated)
            if rate == 44100 and channels == 1:
                bare = media[1].read_bytes()
                for version, footer in ((2, False), (3, False), (4, True)):
                    body = b'ignored-tag\0\0\0'
                    header = b'ID3' + bytes([version, 0, 16 if footer else 0, 0, 0, 0, len(body)])
                    tag = header + body + (b'3DI' + header[3:] if footer else b'')
                    path = output / f'id3v{version}.mp3'
                    path.write_bytes(tag + bare + b'TAG' + bytes(125))
                    check(path)
                    if footer:
                        path = output / 'invalid-footer.mp3'
                        path.write_bytes(header + body + b'XDI' + header[3:] + bare)
                        reject(path)
    # Exercise MP4 video normalization through the unified adapter as well.
    mp4 = output / 'video.mp4'
    helper.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i', 'color=c=blue:s=1280x720:r=30',
                '-f', 's16le', '-ar', '48000', '-ac', '2', '-i', output / '48000-2.s16',
                '-frames:v', '10', '-shortest', '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
                '-refs', '1', '-bf', '0', '-pix_fmt', 'yuv420p', '-c:a', 'aac', mp4])
    assert check(mp4)['video'] == 10
    mp4_helper = load('test-player-mp4')
    for rate in ('24', '25', '30000/1001', '30'):
        path = output / ('video-timestamps-' + rate.replace('/', '-') + '.mp4')
        helper.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                    f'color=c=blue:s=1280x720:r={rate}:d=1.2',
                    '-f', 'lavfi', '-i', 'sine=f=997:r=48000:d=1.2', '-ac', '2',
                    '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
                    '-refs', '1', '-bf', '0', '-pix_fmt', 'yuv420p',
                    '-video_track_timescale', '90000', '-movie_timescale', '1000', '-c:a', 'aac', path])
        check(path)
    quantized = output / 'video-timestamps-33-34ms.mp4'
    quantized.write_bytes(mp4_helper.quantized_frame_times(path.read_bytes()))
    check(quantized)
    # Decode every AU, while displaying only the edited presentation window.
    def edited_video(data, *, start=0, duration=0):
        def track(tag, payload):
            if tag == b'trak' and b'vide' in payload:
                def edit(child, body):
                    if child == b'elst':
                        assert body[0] == 0
                        ticks, media, fixed_rate = struct.unpack_from('>IiI', body, 8)
                        body = body[:8] + struct.pack('>IiI', duration or ticks, start or media, fixed_rate)
                    return child, body
                payload = mp4_helper.rewrite(payload, edit)
            return tag, payload
        return mp4_helper.rewrite(data, track)
    for name, changed in (
            ('video-preroll.mp4', edited_video(path.read_bytes(), start=3000, duration=1000)),
            ('video-trimmed-tail.mp4', edited_video(path.read_bytes(), duration=500))):
        invalid = output / name
        invalid.write_bytes(changed)
        check(invalid, video_trim=True)
    bframes = output / 'video-bframes.mp4'
    helper.run([ffmpeg, '-v', 'error', '-y', '-f', 'lavfi', '-i',
                'testsrc2=s=1280x720:r=30:d=1.2', '-f', 'lavfi', '-i',
                'sine=f=997:r=48000:d=1.2', '-c:v', 'libx264', '-profile:v', 'main',
                '-level:v', '3.1', '-refs', '3', '-bf', '2', '-pix_fmt', 'yuv420p',
                '-video_track_timescale', '90000', '-movie_timescale', '1000', '-c:a', 'aac', bframes])
    check(bframes)
    signed = output / 'video-bframes-signed.mp4'
    helper.run([ffmpeg, '-v', 'error', '-y', '-i', bframes, '-c', 'copy',
                '-movflags', '+negative_cts_offsets', signed])
    check(signed)
    for name, changed in (
            ('video-bframes-preroll.mp4', edited_video(bframes.read_bytes(), start=12000, duration=1000)),
            ('video-bframes-trimmed-tail.mp4', edited_video(bframes.read_bytes(), duration=500))):
        path = output / name
        path.write_bytes(changed)
        check(path, video_trim=True)
    if args.probe_media:
        check(args.probe_media)
    (output / 'comparison.json').write_text(json.dumps(reports, indent=2) + '\n', encoding='utf-8')
    print(f'PASS unified source: {len(reports)} real files, AU equality, trim/PCM/backpressure/EOF and failure boundaries; no board test', flush=True)


if __name__ == '__main__':
    main()
