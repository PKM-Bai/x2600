"""Test the freshly generated native .sav reload helper without board access."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'build/regba-host/native_reload')
    parser.add_argument('--vcvars', type=Path, default=Path(
        'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    generated = output / 'regba-core'
    subprocess.run([sys.executable, str(root / 'scripts/prepare-regba.py'),
                    '--source', str(root / 'third_party/ReGBA'), '--output', str(generated)], check=True)
    source = (generated / 'memory.c').read_text(encoding='latin1')
    functions = re.findall(r'(?ms)^int regba_reload_backup\(void\)\n\{.*?^\}', source)
    if len(functions) != 1:
        raise RuntimeError('Expected exactly one freshly generated native reload helper')
    template = (root / 'tests/native_reload_test.c.in').read_text(encoding='utf-8')
    if template.count('@NATIVE_RELOAD_FUNCTION@') != 1:
        raise RuntimeError('Native reload template must have exactly one helper marker')
    fixture = output / 'native_reload_test.c'
    fixture.write_text(template.replace('@NATIVE_RELOAD_FUNCTION@', functions[0]), encoding='utf-8')
    if not args.vcvars.is_file():
        raise RuntimeError('MSVC environment script missing: ' + str(args.vcvars))
    executable = output / 'native_reload_test.exe'
    commands = ['@echo off', f'call "{args.vcvars}" >nul', 'if errorlevel 1 exit /b 1',
                f'cl /nologo /std:c11 /utf-8 /O2 /W4 /WX "{fixture}" '
                f'/Fo"{output / "native_reload_test.obj"}" /Fe"{executable}"',
                'if errorlevel 1 exit /b 1', f'"{executable}"', 'if errorlevel 1 exit /b 1']
    runner = output / 'run-native-reload.cmd'
    runner.write_text('\r\n'.join(commands) + '\r\n', encoding='utf-8')
    subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/s', '/c', str(runner)], check=True, cwd=root)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
