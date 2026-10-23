"""Validate the standalone MIPS AI ELF/BIN; do not deploy hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def verify(directory):
    elf = (directory / 'halley7m_ai.elf').read_bytes()
    binary = (directory / 'halley7m_ai.bin').read_bytes()
    if elf[:7] != b'\x7fELF\x01\x01\x01' or len(elf) < 52:
        raise ValueError('expected ELF32 little-endian')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', elf)
    if header[2] != 8 or header[4] != 0x80600000:
        raise ValueError('wrong MIPS machine or application entry')
    if not binary or len(binary) > 6 * 1024 * 1024:
        raise ValueError('RAM upload window exceeded')
    segments = []
    for index in range(header[10]):
        ptype, offset, address, physical, filesz, memsz, flags, align = struct.unpack_from(
            '<IIIIIIII', elf, header[5] + index * header[9])
        if ptype != 1:
            continue
        if address < 0x80600000 or address + memsz > 0x83c00000 or filesz > memsz:
            raise ValueError('segment outside 54 MiB application region')
        relative = address - 0x80600000
        if elf[offset:offset+filesz] != binary[relative:relative+filesz]:
            raise ValueError('ELF/BIN segment mismatch')
        segments.append({'address': hex(address), 'file_bytes': filesz, 'memory_bytes': memsz})
    if not segments:
        raise ValueError('ELF has no loadable segments')
    result = {'schema': 1, 'project': 'halley7m-ai', 'entry': '0x80600000',
              'bytes': len(binary), 'sha256': hashlib.sha256(binary).hexdigest(),
              'nor_slot_bytes': 2326528, 'fits_existing_nor_slot': len(binary) <= 2326528,
              'segments': segments, 'board_verified': False, 'nor_installed': False}
    (directory / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))
    return result

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    verify(parser.parse_args().build)
