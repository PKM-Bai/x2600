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
                SOURCE.parent / 'codec', SOURCE.parent / 'display', SOURCE.parent / 'storage']
    environment = os.environ.copy()
    if os.name == 'nt':
        spec = importlib.util.spec_from_file_location('amp_support', ROOT / 'scripts/test-player-amp.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        environment = support.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
        # ESTALE is POSIX/newlib; MSVC's errno.h does not expose it.
        base = [compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/wd4324', '/utf-8',
                '/DESTALE=116', '/DPLAYER_SESSION_HOST_TEST']
        base += ['/I' + str(path) for path in includes]
    else:
        base = [os.environ.get('CC', 'gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-DPLAYER_SESSION_HOST_TEST']
        for path in includes:
            base += ['-I', str(path)]
    common = SOURCE.parent / 'common'
    groups = {
        'session_core_test': [SOURCE / 'tests/session_core_test.c', common / 'player_session_core.c'],
        'session_test': [SOURCE / 'tests/session_test.c', SOURCE / 'player_session.c',
                         common / 'player_session_core.c', common / 'player_media.c', common / 'player_video.c'],
    }
    executables = []
    for name, sources in groups.items():
        executable = output / (name + ('.exe' if os.name == 'nt' else ''))
        command = base + [str(path) for path in sources]
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
    print('Production session/core host tests passed; no physical storage, serial or hardware access.')


if __name__ == '__main__':
    main()
