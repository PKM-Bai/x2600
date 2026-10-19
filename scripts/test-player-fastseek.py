#!/usr/bin/env python3
"""Build production derived FatFs/adapter against a virtual >2GiB FAT32 disk."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / 'projects/halley7m-player/storage'
    output = ROOT / 'build/player-host/fastseek'
    output.mkdir(parents=True, exist_ok=True)
    ssi = output / 'ssi'
    derived = output / 'derived'
    subprocess.run([sys.executable, str(ROOT / 'scripts/prepare-ssi.py'), '--sdk', str(ROOT / 'libbare-cpu'),
                    '--output', str(ssi)], check=True)
    subprocess.run([sys.executable, str(source / 'prepare-storage.py'), '--output', str(derived),
                    '--ssi', str(ssi / 'x2600_hal_ssi.c')], check=True)
    includes = [derived / 'fatfs', source / 'tests', source, source.parent / 'common',
                ROOT / 'projects/halley7m-regba/platform']
    own = [ROOT / 'tests/player_storage_fastseek_test.c', source / 'player_storage.c',
           ROOT / 'projects/halley7m-regba/platform/path_encoding.c']
    vendor = [derived / 'fatfs/ff.c', derived / 'fatfs/unicode.c']
    executable = output / ('fastseek_test.exe' if os.name == 'nt' else 'fastseek_test')
    if os.name == 'nt':
        spec = importlib.util.spec_from_file_location('support', ROOT / 'scripts/test-player-amp.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        environment = support.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
        common = [compiler, '/nologo', '/std:c11', '/O2', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS']
        common += ['/I' + str(path) for path in includes]
        for files, warnings in ((own, ['/W4', '/WX']), (vendor, ['/W0'])):
            subprocess.run(common + warnings + ['/c'] + [str(path) for path in files],
                           cwd=output, env=environment, check=True)
        subprocess.run(common + [str(output / (path.stem + '.obj')) for path in own + vendor] +
                       ['/Fe:' + str(executable)], cwd=output, env=environment, check=True)
    else:
        command = [os.environ.get('CC', 'gcc'), '-std=c11', '-O2']
        for path in includes:
            command += ['-I', str(path)]
        subprocess.run(command + [str(path) for path in own + vendor] + ['-o', str(executable)],
                       cwd=output, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
