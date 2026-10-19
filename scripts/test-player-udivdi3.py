"""Compare the production RV32 __udivdi3 with native 64-bit unsigned division."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    runtime = ROOT / 'projects/halley7m-player/riscv/runtime.c'
    functions = re.findall(r'(?ms)^uint64_t __udivdi3\(.*?^}', runtime.read_text(encoding='utf-8'))
    if len(functions) != 1:
        raise RuntimeError('Expected one production unsigned division helper')
    production = '#include <stdint.h>\n' + functions[0] + '\n'
    template = (ROOT / 'tests/player_udivdi3_test.c').read_text(encoding='utf-8')
    if template.count('/* @PRODUCTION_UDIVDI3@ */') != 1:
        raise RuntimeError('Expected one test injection marker')
    output = ROOT / 'build/player-host/udivdi3'
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'udivdi3_test.c'
    generated.write_text(template.replace('/* @PRODUCTION_UDIVDI3@ */', production), encoding='utf-8')
    # This translation unit is also the exact input for target helper audits.
    (output / 'udivdi3_production.c').write_text(production, encoding='utf-8')
    spec = importlib.util.spec_from_file_location('amp_test_support', ROOT / 'scripts/test-player-amp.py')
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    environment = support.msvc_environment()
    compiler = shutil.which('cl', path=environment.get('Path', environment.get('PATH'))) or 'cl'
    for optimization in ('/Od', '/O2'):
        executable = output / ('udivdi3_debug.exe' if optimization == '/Od' else 'udivdi3_release.exe')
        subprocess.run([compiler, '/nologo', '/std:c11', optimization, '/W4', '/WX', '/utf-8',
                        str(generated), '/Fe:' + str(executable)], cwd=output, env=environment, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
