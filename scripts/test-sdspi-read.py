"""Run production SD SPI functions against a native in-memory card model."""

import argparse
import os
from pathlib import Path
import re
import subprocess


def masked_c(source):
    """Keep source offsets while hiding braces in strings and comments."""
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda match: ''.join('\n' if c == '\n' else ' ' for c in match[0]),
                  source, flags=re.S)


def extract_function(source, name):
    masked = masked_c(source)
    pattern = rf'(?m)^(?:static\s+)?(?:int|void|uint(?:8|16|32)_t)\s+{re.escape(name)}\s*\([^;{{]*\)\s*\{{'
    matches = list(re.finditer(pattern, masked))
    if len(matches) != 1:
        raise RuntimeError(f'Expected one production function {name}, found {len(matches)}')
    start = matches[0].start()
    depth = 1
    for offset in range(matches[0].end(), len(masked)):
        if masked[offset] == '{':
            depth += 1
        elif masked[offset] == '}':
            depth -= 1
            if not depth:
                return source[start:offset + 1]
    raise RuntimeError('Unclosed production function ' + name)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / 'build/regba-host/sdspi')
    parser.add_argument('--vcvars', type=Path,
                        default=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = (root / 'projects/halley7m-regba/platform/sdspi.c').read_text(encoding='utf-8')
    trace = re.findall(r'(?ms)^typedef struct \{.*?^\} sdspi_trace_t;\s*\nstatic sdspi_trace_t trace;', source)
    if len(trace) != 1:
        raise RuntimeError('Expected exactly one production SD trace type and instance')
    names = ('sdspi_crc7', 'sdspi_crc16', 'trace_begin', 'read_byte', 'deselect',
             'ready', 'select_card', 'command', 'capture_illegal_token',
             'receive_data', 'stop_read', 'set_data_speed', 'bounds', 'sdspi_read', 'sdspi_write', 'sdspi_sync')
    functions = {name: extract_function(source, name) for name in names}
    template = (root / 'tests/sdspi_read_test.c.in').read_text(encoding='utf-8')
    for marker in ('@TRACE_TYPE@', '@PRODUCTION_FUNCTIONS@'):
        if template.count(marker) != 1:
            raise RuntimeError('Expected exactly one fixture marker ' + marker)
    variants = {'production': functions}
    no_stuff = dict(functions)
    pattern = r'\(void\)read_byte\(\);'
    if len(re.findall(pattern, functions['command'])) != 1:
        raise RuntimeError('CMD12 negative control requires exactly one discarded byte')
    no_stuff['command'] = re.sub(pattern, '(void)0;', functions['command'], count=1)
    variants['without_cmd12_stuff'] = no_stuff
    bad_crc = dict(functions)
    if functions['sdspi_crc16'].count('return crc;') != 1:
        raise RuntimeError('CRC negative control requires exactly one CRC return')
    bad_crc['sdspi_crc16'] = functions['sdspi_crc16'].replace(
        'return crc;', 'volatile uint16_t fault_mask = 1; return (uint16_t)(crc ^ fault_mask);', 1)
    variants['broken_crc16'] = bad_crc
    for name, parts in variants.items():
        generated = template.replace('@TRACE_TYPE@', trace[0]).replace(
            '@PRODUCTION_FUNCTIONS@', '\n\n'.join(parts[part] for part in names))
        (output / (name + '.c')).write_text(generated, encoding='utf-8')
    if not args.vcvars.is_file():
        raise RuntimeError('MSVC environment script missing: ' + str(args.vcvars))
    commands = ['@echo off', f'call "{args.vcvars}" >nul', 'if errorlevel 1 exit /b 1']
    for name in variants:
        commands += [f'cl /nologo /std:c11 /utf-8 /O2 /W4 /WX "{output / (name + ".c")}" '
                     f'/Fo"{output / (name + ".obj")}" /Fe"{output / (name + ".exe")}"',
                     'if errorlevel 1 exit /b 1']
    runner = output / 'compile.cmd'
    runner.write_text('\r\n'.join(commands) + '\r\n', encoding='utf-8')
    subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/d', '/s', '/c', str(runner)],
                   check=True, cwd=root)
    result = subprocess.run([str(output / 'production.exe')], text=True, capture_output=True)
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise RuntimeError('Production SD fixture failed: ' + result.stderr)
    for name, expected in (('without_cmd12_stuff', 'CMD18 multi-block read'),
                           ('broken_crc16', 'CRC16 oracle')):
        result = subprocess.run([str(output / (name + '.exe'))], text=True, capture_output=True)
        message = result.stdout + result.stderr
        if result.returncode != 1 or expected not in message:
            raise RuntimeError(f'Negative control {name} did not fail its target assertion: {message}')
        print(f'Expected RED {name}: {message.strip().splitlines()[-1]}', flush=True)
    for changed in (source.replace('int sdspi_read(', 'int missing_sdspi_read(', 1),
                    source + '\n' + functions['sdspi_read']):
        try:
            extract_function(changed, 'sdspi_read')
        except RuntimeError:
            continue
        raise RuntimeError('Function extractor accepted missing/ambiguous production read')
    print('PASS: SD extraction rejects missing/ambiguous functions; negative controls verify stuff-byte and CRC coverage', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
