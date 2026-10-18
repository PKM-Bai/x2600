"""Compile actual ReGBA frame callbacks and playback helpers in a native fixture."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def extract_function(source, name):
    # Preserve offsets while hiding comments and strings that can contain braces.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]),
                    source, flags=re.S)
    pattern = (rf'(?m)^(?:static\s+)?(?:void|bool|int16_t|signed\s+int)\s+'
               rf'{re.escape(name)}\s*\([^;{{]*\)\s*\{{')
    matches = list(re.finditer(pattern, masked))
    if len(matches) != 1:
        raise RuntimeError(f'Expected exactly one production {name}, found {len(matches)}')
    match = matches[0]
    depth = 1
    for offset in range(match.end(), len(masked)):
        depth += (masked[offset] == '{') - (masked[offset] == '}')
        if not depth:
            return source[match.start():offset + 1]
    raise RuntimeError('Unclosed production function ' + name)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'build/regba-host/playback')
    parser.add_argument('--vcvars', type=Path,
                        default=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    core = root / 'projects/halley7m-regba/core'
    source = (core.parent / 'engines/regba/regba_adapter.c').read_text(encoding='utf-8')
    names = ('clamp12', 'reset_audio_update_timing', 'ReGBA_AudioUpdate',
             'ReGBA_RenderScreen', 'ReGBA_IsRenderingNextFrame', 'regba_frame_boundary')
    functions = '\n\n'.join(extract_function(source, name) for name in names)
    template = (root / 'tests/playback_test.c.in').read_text(encoding='utf-8')
    if template.count('@PRODUCTION_FUNCTIONS@') != 1:
        raise RuntimeError('Playback fixture needs exactly one production function marker')
    generated = output / 'playback_test.c'
    generated.write_text(template.replace('@PRODUCTION_FUNCTIONS@', functions), encoding='utf-8')
    if not args.vcvars.is_file():
        raise RuntimeError('MSVC environment script missing: ' + str(args.vcvars))
    executable = output / 'playback_test.exe'
    commands = ['@echo off', f'call "{args.vcvars}" >nul', 'if errorlevel 1 exit /b 1',
                f'cl /nologo /std:c11 /utf-8 /O2 /W4 /WX /I"{core}" "{generated}" '
                f'"{core / "playback.c"}" /Fo"{output}\\\\" /Fe"{executable}"',
                'if errorlevel 1 exit /b 1']
    runner = output / 'compile.cmd'
    runner.write_text('\r\n'.join(commands) + '\r\n', encoding='utf-8')
    subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/s', '/c', str(runner)],
                   check=True, cwd=root)
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    (output / 'results.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise RuntimeError('Production playback fixture failed:\n' + result.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
