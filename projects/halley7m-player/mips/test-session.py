"""Run production session/core code against deterministic storage/audio/DMA peers."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
from fractions import Fraction

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent


def probe(ffprobe, path):
    return json.loads(subprocess.check_output([str(ffprobe), '-v', 'error', '-show_streams',
                                              '-show_packets', '-of', 'json', str(path)]))


def reference_video(metadata):
    video = next(stream for stream in metadata['streams'] if stream['codec_type'] == 'video')
    scale = Fraction(video['time_base'])
    packets = [packet for packet in metadata['packets'] if packet['stream_index'] == video['index']]
    return [(Fraction(int(packet['pts'])) * scale, Fraction(int(packet['duration'])) * scale)
            for packet in packets]


def ceiling(value):
    return (value.numerator + value.denominator - 1) // value.denominator


def run_session(arguments, duration_us):
    environment = os.environ.copy()
    environment['PLAYER_TEST_DURATION_US'] = str(duration_us)
    subprocess.run(list(map(str, arguments)), env=environment, check=True)


def video_edit(data, support, *, shift_ticks=0, duration_ms=None, remove=False):
    """Fixtures use movie scale 1000 and video scale 90000; keep every AU."""
    previous_start = None
    def track(tag, payload):
        nonlocal previous_start
        if tag == b'trak' and b'vide' in payload:
            def edit(child, body):
                nonlocal previous_start
                if child == b'elst':
                    assert body[0] == 0
                    duration, start, rate = struct.unpack_from('>IiI', body, 8)
                    previous_start = start
                    if remove:
                        return b'free', body
                    body = body[:8] + struct.pack('>IiI', duration if duration_ms is None else duration_ms,
                                                  start + shift_ticks, rate)
                return child, body
            payload = support.rewrite(payload, edit)
        return tag, payload
    changed = support.rewrite(data, track)
    assert previous_start is not None
    return changed, previous_start


def build_host(output=None):
    output = Path(output) if output is not None else ROOT / 'build/player-host/session'
    output.mkdir(parents=True, exist_ok=True)
    includes = [SOURCE / 'tests', SOURCE, SOURCE.parent / 'common',
                SOURCE.parent / 'codec', SOURCE.parent / 'display', SOURCE.parent / 'storage',
                ROOT / 'projects/halley7m-player/audio-decode']
    environment = os.environ.copy()
    if os.name == 'nt':
        spec = importlib.util.spec_from_file_location('amp_support', ROOT / 'scripts/test-player-amp.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        environment = support.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
        # ESTALE is POSIX/newlib; MSVC's errno.h does not expose it.
        base = [compiler, '/nologo', '/std:c11', '/O2', '/MD', '/W4', '/WX', '/wd4324', '/utf-8',
                '/DESTALE=116', '/DPLAYER_SESSION_HOST_TEST']
        base += ['/I' + str(path) for path in includes]
    else:
        base = [os.environ.get('CC', 'gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-DPLAYER_SESSION_HOST_TEST']
        for path in includes:
            base += ['-I', str(path)]
    common = SOURCE.parent / 'common'
    # Compile the real codec libraries so compressed lifecycle cases exercise
    # the same PCM trimming/resampling path as the MIPS session.
    decode_build = output / 'audio-decode'
    subprocess.run(['cmake', '-S', str(ROOT / 'projects/halley7m-player/audio-decode'), '-B', str(decode_build),
                    '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=',
                    '-DCMAKE_C_FLAGS_DEBUG=', '-DCMAKE_C_FLAGS_RELEASE=/O2 /DNDEBUG' if os.name == 'nt' else '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG'],
                   env=environment, check=True)
    subprocess.run(['cmake', '--build', str(decode_build), '--parallel', '4'], env=environment, check=True)
    libs = [decode_build / n for n in (('player_audio_decode.lib', 'player_faad_fixed.lib') if os.name == 'nt'
                                     else ('libplayer_audio_decode.a', 'libplayer_faad_fixed.a'))]
    groups = {
        'session_core_test': [SOURCE / 'tests/session_core_test.c', common / 'player_session_core.c'],
        'session_test': [SOURCE / 'tests/session_test.c', SOURCE / 'player_session.c',
                         common / 'player_session_core.c', common / 'player_media.c', common / 'player_video.c',
                         common / 'player_source.c', common / 'player_mp4.c',
                         SOURCE.parent / 'display/player_display_format.c'],
    }
    executables = []
    for name, sources in groups.items():
        executable = output / (name + ('.exe' if os.name == 'nt' else ''))
        command = base + [str(path) for path in sources]
        if name == 'session_test':
            command += [str(p) for p in libs]
            if os.name != 'nt': command.append('-lm')
        command += ['/Fe:' + str(executable)] if os.name == 'nt' else ['-o', str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True)
        executables.append(executable)
    return output, executables, base, libs, environment


def main():
    output, executables, _, _, _ = build_host()
    subprocess.run([str(executables[0])], check=True)
    ffmpeg = ROOT / 'tools/ffmpeg/bin' / ('ffmpeg.exe' if os.name == 'nt' else 'ffmpeg')
    if not ffmpeg.is_file():
        ffmpeg = Path(shutil.which('ffmpeg') or '')
    if not ffmpeg.is_file():
        raise RuntimeError('FFmpeg is required to generate the controlled real AVI fixture')
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    for duration, frames in [('0.4', 12), ('3', 90)]:
        fixture = output / f'session-fixture-{frames}.avi'
        subprocess.run([str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y',
            '-f', 'lavfi', '-i', f'testsrc2=size=720x1280:rate=30:duration={duration}',
            '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=mono', '-t', duration,
            '-map', '0:v', '-map', '1:a', '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
            '-pix_fmt', 'yuv420p', '-refs', '1', '-bf', '0', '-g', '30',
            '-x264-params', 'cabac=0:repeat-headers=1:aud=1:scenecut=0',
            '-c:a', 'pcm_s16le', '-ar', '48000', '-ac', '1', '-f', 'avi', str(fixture)], check=True)
        # The third argument selects a periodic 33.033 ms EOD, rather than the
        # fast 1 ms peer used by the short lifecycle cases.
        arguments = [str(executables[1]), str(fixture)]
        if frames == 90:
            arguments.append(str(frames))
        run_session(arguments, frames * 1000000 // 30)
    # Exact source frame counts after MP3 encoder delay/padding and M4A elst;
    # bare ADTS keeps its padding because it carries no edit duration.
    for suffix, codec_args, expected in [
        ('mp3', ['-c:a', 'libmp3lame'], 48000),
        ('aac', ['-c:a', 'aac', '-f', 'adts'], 44 * 1024 * 48000 // 44100),
        ('m4a', ['-c:a', 'aac'], 48000),
        ('mp4', ['-c:a', 'aac'], 48000),
    ]:
        fixture = output / ('compressed.' + suffix)
        command = [str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-y']
        if suffix == 'mp4':
            command += ['-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=30:duration=1']
        command += ['-f', 'lavfi', '-i', 'sine=frequency=500:sample_rate=44100:duration=1', '-t', '1']
        if suffix == 'mp4':
            command += ['-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1', '-refs', '1',
                        '-bf', '0', '-g', '30', '-pix_fmt', 'yuv420p']
        command += codec_args + ['-ac', '2', str(fixture)]
        subprocess.run(command, check=True)
        run_session([executables[1], fixture, '30' if suffix == 'mp4' else '0', expected],
                    0 if suffix == 'aac' else 1000000)
    # The real session must follow packet PTS rather than reconstructing 30fps.
    spec = importlib.util.spec_from_file_location('mp4_support', ROOT / 'scripts/test-player-mp4.py')
    mp4_support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mp4_support)
    for rate, frames in [('24', 24), ('25', 25), ('30000/1001', 30), ('30', 30)]:
        fixture = output / ('pts-' + rate.replace('/', '-') + '.mp4')
        subprocess.run([str(ffmpeg), '-v', 'error', '-y', '-f', 'lavfi', '-i',
            f'testsrc2=size=1280x720:rate={rate}:duration=1',
            '-f', 'lavfi', '-i', 'sine=frequency=500:sample_rate=48000:duration=1',
            '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1', '-refs', '1',
            '-bf', '0', '-pix_fmt', 'yuv420p', '-video_track_timescale', '90000',
            '-c:a', 'aac', '-ac', '2', str(fixture)], check=True)
        video = reference_video(probe(ffprobe, fixture))
        expected_samples = max(48000, ceiling(max(pts + duration for pts, duration in video) * 48000))
        duration_us = max(1000000, int(max(pts + duration for pts, duration in video) * 1000000))
        run_session([executables[1], fixture, frames, expected_samples], duration_us)
    fixture = output / 'pts-33-34ms.mp4'
    fixture.write_bytes(mp4_support.quantized_frame_times((output / 'pts-30.mp4').read_bytes()))
    run_session([executables[1], fixture, '30', '48000'], 1000000)
    # PTS order intentionally differs from AU order. The fake codec owns
    # delayed slots until reorder eligibility, and EOF must drain the tail.
    cases = []
    for profile, refs, bframes, depth in [('main', 3, 2, 2), ('high', 6, 4, 4)]:
        fixture = output / f'bframes-{profile}-ref{refs}-b{bframes}.mp4'
        subprocess.run([str(ffmpeg), '-v', 'error', '-y', '-f', 'lavfi', '-i',
            'testsrc2=size=1280x720:rate=30:duration=1.2', '-f', 'lavfi', '-i',
            'sine=frequency=500:sample_rate=48000:duration=1.2',
            '-c:v', 'libx264', '-profile:v', profile, '-level:v', '4.0', '-refs', str(refs),
            '-bf', str(bframes), '-pix_fmt', 'yuv420p', '-g', '30',
            '-x264-params', 'b-adapt=0:scenecut=0', '-video_track_timescale', '90000',
            '-movie_timescale', '1000', '-c:a', 'aac', '-ac', '2', str(fixture)], check=True)
        video = reference_video(probe(ffprobe, fixture))
        assert any(a[0] > b[0] for a, b in zip(video, video[1:]))
        cases.append((fixture, video, depth, Fraction(6, 5)))
        if profile == 'main':
            signed = output / 'bframes-main-signed-ctts.mp4'
            subprocess.run([str(ffmpeg), '-v', 'error', '-y', '-i', str(fixture), '-c', 'copy',
                            '-movflags', '+negative_cts_offsets', '-movie_timescale', '1000', str(signed)], check=True)
            signed_video = reference_video(probe(ffprobe, signed))
            assert signed_video == video
            cases.append((signed, signed_video, depth, Fraction(6, 5)))
            for name, shift, milliseconds in [('preroll', 3000, 1000), ('tail', 0, 500)]:
                edited = output / f'bframes-main-{name}.mp4'
                changed, _ = video_edit(fixture.read_bytes(), mp4_support,
                                        shift_ticks=shift, duration_ms=milliseconds)
                edited.write_bytes(changed)
                timeline = [(pts - Fraction(shift, 90000), duration) for pts, duration in video]
                cases.append((edited, timeline, depth, Fraction(milliseconds, 1000)))
            delayed = output / 'bframes-main-positive-first-pts.mp4'
            changed, previous_start = video_edit(fixture.read_bytes(), mp4_support, remove=True)
            delayed.write_bytes(changed)
            timeline = [(pts + Fraction(previous_start, 90000), duration) for pts, duration in video]
            cases.append((delayed, timeline, depth, max(pts + duration for pts, duration in timeline)))
    short = output / 'bframes-main-short-audio.mp4'
    subprocess.run([str(ffmpeg), '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=1280x720:rate=30:duration=1.2', '-f', 'lavfi', '-i',
        'sine=frequency=500:sample_rate=48000:duration=0.4', '-c:v', 'libx264',
        '-profile:v', 'main', '-level:v', '3.1', '-refs', '3', '-bf', '2', '-pix_fmt', 'yuv420p',
        '-x264-params', 'b-adapt=0:scenecut=0', '-video_track_timescale', '90000',
        '-movie_timescale', '1000', '-c:a', 'aac', '-ac', '2', str(short)], check=True)
    cases.append((short, reference_video(probe(ffprobe, short)), 2, Fraction(6, 5)))
    references = []
    for path, timeline, depth, end in cases:
        visible = [(index, pts) for index, (pts, duration) in enumerate(timeline)
                   if pts + duration > 0 and pts < end]
        last_decode_index = max(visible, key=lambda row: row[1])[0]
        # The fixture AAC has an ordinary elst. FFmpeg's actual decoded PCM
        # count independently verifies priming/trim; pad only the video gap.
        pcm = subprocess.check_output([str(ffmpeg), '-v', 'error', '-i', str(path), '-map', '0:a:0',
                                       '-ar', '48000', '-ac', '1', '-f', 's16le', '-'])
        metadata = probe(ffprobe, path)
        track = next(stream for stream in metadata['streams'] if stream['codec_type'] == 'audio')
        audio_duration = int(track['duration_ts']) * Fraction(track['time_base'])
        edited_samples = int(audio_duration * 48000)
        audio_samples = min(len(pcm) // 2, edited_samples)
        expected = max(audio_samples, ceiling(end * 48000))
        references.append({'name': path.name, 'decode_frames': len(timeline), 'visible_frames': len(visible),
                           'last_display_decode_index': last_decode_index, 'audio_media_samples': audio_samples,
                           'audio_total_samples': expected, 'audio_padding_samples': expected - audio_samples,
                           'reorder_depth': depth})
        run_session([executables[1], path, len(timeline), expected,
                     depth, last_decode_index, len(visible), expected - audio_samples],
                    int(max(end, audio_duration) * 1000000))
    (output / 'bframes-reference.json').write_text(json.dumps(references, indent=2) + '\n', encoding='utf-8')
    print('Production session/core host tests passed; no physical storage, serial or hardware access.')


if __name__ == '__main__':
    main()
