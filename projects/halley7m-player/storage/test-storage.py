"""Test the real player storage adapter against an in-memory FatFs API peer."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    source = Path(__file__).resolve().parent
    output = ROOT / 'build/player-host/storage'
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ('storage_test.exe' if os.name == 'nt' else 'storage_test')
    sources = [source / 'tests/storage_test.c', source / 'player_storage.c',
               ROOT / 'projects/halley7m-regba/platform/path_encoding.c']
    includes = [source / 'tests', source, source.parent / 'common',
                ROOT / 'projects/halley7m-regba/platform']
    if os.name == 'nt':
        spec = importlib.util.spec_from_file_location('amp_test_support', ROOT / 'scripts/test-player-amp.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        environment = support.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
        command = [compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8']
        command += ['/I' + str(path) for path in includes]
        command += [str(path) for path in sources] + ['/Fe:' + str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True)
    else:
        command = [os.environ.get('CC', 'gcc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
        for path in includes:
            command += ['-I', str(path)]
        command += [str(path) for path in sources] + ['-o', str(executable)]
        subprocess.run(command, cwd=output, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
