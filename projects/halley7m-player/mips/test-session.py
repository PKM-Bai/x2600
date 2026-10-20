"""Run production session/core code against deterministic storage/audio/DMA peers."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SOURCE = Path(__file__).resolve().parent


def main():
    output = ROOT / 'build/player-host/session'
    output.mkdir(parents=True, exist_ok=True)
    includes = [SOURCE / 'tests', SOURCE, SOURCE.parent / 'common',
                SOURCE.parent / 'codec', SOURCE.parent / 'display', SOURCE.parent / 'storage',
                SOURCE.parent / 'audio-decode']
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
    decode_build = ROOT / 'build/player-session-audio-host'
    subprocess.run(['cmake', '-S', str(SOURCE.parent / 'audio-decode'), '-B', str(decode_build),
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
                         common / 'player_source.c', common / 'player_mp4.c'],
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
    subprocess.run([str(executables[0])], check=True)
    ffmpeg = ROOT / 'tools/ffmpeg/bin' / ('ffmpeg.exe' if os.name == 'nt' else 'ffmpeg')
    if not ffmpeg.is_file():
        ffmpeg = Path(shutil.which('ffmpeg') or '')
    if not ffmpeg.is_file():
        raise RuntimeError('FFmpeg is required to generate the controlled real AVI fixture')
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
        subprocess.run(arguments, check=True)
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
        subprocess.run([str(executables[1]), str(fixture), '30' if suffix == 'mp4' else '0', str(expected)], check=True)
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
        subprocess.run([str(executables[1]), str(fixture), str(frames), '48000'], check=True)
    fixture = output / 'pts-33-34ms.mp4'
    fixture.write_bytes(mp4_support.quantized_frame_times((output / 'pts-30.mp4').read_bytes()))
    subprocess.run([str(executables[1]), str(fixture), '30', '48000'], check=True)
    print('Production session/core host tests passed; no physical storage, serial or hardware access.')


if __name__ == '__main__':
    main()
