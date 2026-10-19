"""Test production CPU-window math and cross-core diagnostic wire access.

This verifies arithmetic and publication, not target scheduler/ISR accounting.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import re

ROOT = Path(__file__).resolve().parents[1]


def main():
    output = ROOT / 'build/player-host/cpu-diagnostics'
    output.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('amp_test_support', ROOT / 'scripts/test-player-amp.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    environment = support.msvc_environment()
    compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
    common = ROOT / 'projects/halley7m-player/common'
    mips = ROOT / 'projects/halley7m-player/mips'
    production = (mips / 'player_diagnostics.c').read_text(encoding='utf-8')
    production = re.sub(r'^#include[^\n]*\n', '', production, flags=re.M)
    template = (ROOT / 'tests/player_diagnostics_poll_test.c').read_text(encoding='utf-8')
    if template.count('/* @PRODUCTION_DIAGNOSTICS@ */') != 1:
        raise RuntimeError('Production diagnostics test marker changed')
    generated = output / 'diagnostics_poll.c'
    generated.write_text(template.replace('/* @PRODUCTION_DIAGNOSTICS@ */', production), encoding='utf-8')
    for name, test in (('cpu_math', ROOT / 'tests/player_cpu_diagnostics_test.c'), ('cpu_poll', generated)):
        executable = output / f'{name}.exe'
        subprocess.run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/wd4324', '/utf-8',
            '/I' + str(common), '/I' + str(mips), str(test),
            str(common / 'player_cpu_diagnostics.c'), str(common / 'player_amp.c'),
            '/Fe:' + str(executable)], cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
