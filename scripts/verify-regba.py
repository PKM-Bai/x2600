"""Check the linked firmware's fixed board bounds and write an artifact manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', required=True, type=Path)
    p.add_argument('--prefix', required=True)
    p.add_argument('--engine', choices=('regba', 'mgba'), default='regba')
    args = p.parse_args()
    suffix = '.exe' if Path(args.prefix + 'nm.exe').is_file() else ''
    elf = args.build / 'halley7m_regba.elf'
    binary = args.build / 'halley7m_regba.bin'
    def tool(name, *options):
        return subprocess.check_output([args.prefix + name + suffix, *options, str(elf)], text=True)
    header = tool('readelf', '-h')
    if not all(value in header for value in ['ELF32', 'little endian', 'o32', 'mips32r2', '0x80600000']):
        raise RuntimeError('ELF does not match the fixed X2600M application ABI/entry')
    if tool('nm', '-u').strip():
        raise RuntimeError('Firmware has unresolved symbols')
    symbols = {}
    for line in tool('nm', '-n').splitlines():
        match = re.match(r'^([0-9a-fA-F]+)\s+\w\s+(\S+)$', line)
        if match: symbols[match[2]] = int(match[1], 16)
    required = ['_start', '__image_copy_end', '__heap_start', '__heap_end']
    regba_symbols = ['readonly_code_cache', 'writable_code_cache', 'ReGBA_MakeCodeVisible']
    if args.engine == 'regba':
        required += regba_symbols
    else:
        required += ['mgba_adapter_backend', 'GBACoreCreate']
        if any(name in symbols for name in regba_symbols):
            raise RuntimeError('mGBA-only firmware retains ReGBA JIT symbols')
    if any(name not in symbols for name in required):
        raise RuntimeError('Expected linker/core symbols missing')
    if symbols['_start'] != 0x80600000 or symbols['__heap_end'] != 0x84000000:
        raise RuntimeError('Application window changed')
    if symbols['__heap_end'] - symbols['__heap_start'] < 36 * 1024 * 1024:
        raise RuntimeError('Static memory leaves less than 32MiB ROM + 4MiB runtime budget')
    if any(name in symbols for name in ['SAVESTATE_REWIND_MEM', 'savestate_write_buffer']):
        raise RuntimeError('Disabled rewind/state buffers are retained')
    data = binary.read_bytes()
    if len(data) > 6 * 1024 * 1024:
        raise RuntimeError('Binary exceeds the NOR application partition')
    manifest = {
        'board': 'HALLEY7-M-KIT X2600M 64MiB DDR2',
        'engine': args.engine,
        'binary': binary.name, 'binary_bytes': len(data),
        'binary_sha256': hashlib.sha256(data).hexdigest(),
        'entry': hex(symbols['_start']),
        'heap_start': hex(symbols['__heap_start']), 'heap_end': hex(symbols['__heap_end']),
        'heap_before_task_allocation_bytes': symbols['__heap_end'] - symbols['__heap_start'],
        'checks': 'ELF ABI/entry, no undefined symbols, NOR size, DDR bounds, disabled state buffers discarded',
        'hardware_validated': False,
    }
    manifest[args.engine + '_commit'] = {
        'regba': '7f379052dc14366e622a96c7cf7fb6b5a7e918a3',
        'mgba': '26b7884bc25a5933960f3cdcd98bac1ae14d42e2',
    }[args.engine]
    (args.build / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f"Artifact verified: {len(data)} bytes; initial heap {manifest['heap_before_task_allocation_bytes']} bytes; hardware validation pending")


if __name__ == '__main__': main()
