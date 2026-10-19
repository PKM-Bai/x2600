"""Compile production AMP shared-memory code and test its wire protocol on the host.

This test does not access a serial port, core-control register or physical DDR.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def msvc_environment():
    if shutil.which('cl'):
        return os.environ.copy()
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = ''
    if vswhere.is_file():
        installation = subprocess.check_output([
            str(vswhere), '-latest', '-products', '*', '-requires',
            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'
        ], text=True).strip()
    if not installation:
        installation = 'C:/Program Files/Microsoft Visual Studio/18/Community'
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not setup.is_file():
        raise RuntimeError('MSVC not found; pass --cc with a working C11 compiler')
    lines = subprocess.check_output(f'call "{setup}" >nul && set', shell=True, text=True).splitlines()
    return dict(line.split('=', 1) for line in lines if '=' in line and not line.startswith('='))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cl' if os.name == 'nt' else 'gcc'))
    args = parser.parse_args()
    output = ROOT / 'build/player-host/amp'
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ('player_amp_test.exe' if os.name == 'nt' else 'player_amp_test')
    source = ROOT / 'projects/halley7m-player/common'
    test = ROOT / 'tests/player_amp_test.c'
    if Path(args.cc).stem.lower() == 'cl':
        environment = msvc_environment()
        compiler = shutil.which(args.cc, path=environment.get('Path', environment.get('PATH'))) or args.cc
        # C4324 reports the deliberate 64-byte ABI/cache-line padding.
        subprocess.run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/wd4324',
                        '/utf-8', '/I' + str(source), str(test), str(source / 'player_amp.c'),
                        '/Fe:' + str(executable)], cwd=output, env=environment, check=True)
    else:
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(source), str(test), str(source / 'player_amp.c'),
                        '-o', str(executable)], cwd=output, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
