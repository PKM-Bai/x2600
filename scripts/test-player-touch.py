"""Build the production GT911 protocol against a bounded fake I2C panel."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cl' if os.name == 'nt' else 'gcc'))
    args = parser.parse_args()
    output = ROOT / 'build/player-host/touch'
    output.mkdir(parents=True, exist_ok=True)
    executable = output / ('player_touch_test.exe' if os.name == 'nt' else 'player_touch_test')
    source = ROOT / 'projects/halley7m-player/touch'
    sources = [source / 'tests/player_touch_test.c', source / 'player_touch_core.c']
    if Path(args.cc).stem.lower() == 'cl':
        spec = importlib.util.spec_from_file_location('touch_test_support', ROOT / 'scripts/test-player-amp.py')
        support = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(support)
        environment = support.msvc_environment()
        compiler = shutil.which(args.cc, path=environment.get('Path', environment.get('PATH'))) or args.cc
        command = [compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8', '/I' + str(source)]
        command += [str(path) for path in sources] + ['/Fe:' + str(executable)]
        subprocess.run(command, cwd=output, env=environment, check=True)
    else:
        subprocess.run([args.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(source),
                        *map(str, sources), '-o', str(executable)], cwd=output, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
