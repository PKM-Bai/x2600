#!/usr/bin/env python3
"""Test the actual appended display sampler's state guards and address bounds."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / 'build/player-display-sample-host'
    output.mkdir(parents=True, exist_ok=True)
    display = ROOT / 'projects/halley7m-player/display'
    production = (display / 'player_display.c').read_text(encoding='utf-8')
    marker = '#include "player_display_sample.h"'
    if production.count(marker) != 1:
        raise RuntimeError('production sampler boundary changed')
    # The fixture supplies only private driver state and a memory read hook.
    # Compile the complete production sampler rather than a copied algorithm.
    (output / 'player_display_sample_derived.inc').write_text(
        production[production.index(marker):], encoding='utf-8')
    source = display / 'tests/player_display_sample_test.c'
    executable = output / ('player_display_sample_test.exe' if os.name == 'nt' else 'player_display_sample_test')
    if os.name == 'nt':
        spec = importlib.util.spec_from_file_location('player_amp_host', ROOT / 'scripts/test-player-amp.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        environment = helper.msvc_environment()
        compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH')))
        if not compiler:
            raise RuntimeError('MSVC compiler not found')
        command = [compiler, '/nologo', '/std:c11', '/W4', '/WX', '/UNDEBUG', '/O2', '/utf-8',
                   '/I' + str(display), '/I' + str(output), str(source), '/Fe:' + str(executable)]
    else:
        environment = os.environ.copy()
        command = [environment.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', '-O2',
                   '-I', str(display), '-I', str(output), str(source), '-o', str(executable)]
    subprocess.run(command, cwd=output, env=environment, check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
