"""Verify both firmware ABIs, fixed DDR windows and the compact embedded image.

Reads local artifacts only. A manifest always records hardware_validated=false;
board evidence belongs to a separate validation record.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import zlib

MIPS_BASE, MIPS_END = 0x80600000, 0x83C00000
RISCV_BASE, RISCV_END, RISCV_ENTRY = 0x03C00000, 0x03E00000, 0x03C00080
SHARED_BASE, DDR_END = 0x03E00000, 0x04000000
MAX_PACKAGE = 6 * 1024 * 1024
PROJECT = Path(__file__).resolve().parents[1] / 'projects/halley7m-player'


def source_version(filename, macro):
    text = (PROJECT / 'common' / filename).read_text(encoding='utf-8')
    match = re.search(r'^#define\s+' + re.escape(macro) + r'\s+UINT32_C\((\d+)\)', text, re.M)
    require(match, f'Missing fixed-width version macro: {macro}')
    return int(match[1])


def default_volume():
    text = (PROJECT / 'common/player_config.h').read_text(encoding='utf-8')
    match = re.search(r'^#define\s+PLAYER_DEFAULT_VOLUME_PERCENT\s+(\d+)u\s*$', text, re.M)
    require(match and 0 <= int(match[1]) <= 100, 'Invalid default volume percent')
    return int(match[1])


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Elf32:
    def __init__(self, path):
        self.path = path
        self.data = path.read_bytes()
        require(len(self.data) >= 52 and self.data[:6] == b'\x7fELF\x01\x01',
                f'{path}: expected little-endian ELF32')
        header = struct.unpack_from('<16sHHIIIIIHHHHHH', self.data)
        (_, self.elf_type, self.machine, _, self.entry, phoff, shoff, self.flags,
         _, phsize, phnum, shsize, shnum, _) = header
        require(self.elf_type == 2, f'{path}: expected executable ELF')
        self.loads = []
        for i in range(phnum):
            p = struct.unpack_from('<IIIIIIII', self.data, phoff + i * phsize)
            if p[0] == 1 and p[5]:
                self.loads.append({'vaddr': p[2], 'paddr': p[3], 'file_bytes': p[4],
                                   'memory_bytes': p[5], 'flags': p[6], 'file_offset': p[1]})
        sections = [struct.unpack_from('<IIIIIIIIII', self.data, shoff + i * shsize)
                    for i in range(shnum)]
        self.symbols = {}
        self.undefined = []
        for section in sections:
            if section[1] != 2:
                continue
            strings = sections[section[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                name, value, _, info, _, index = struct.unpack_from('<IIIBBH', self.data, offset)
                end = names.find(b'\0', name)
                label = names[name:end].decode('utf-8', errors='replace')
                if label and index:
                    self.symbols[label] = value
                elif label and (info >> 4) == 1:
                    self.undefined.append(label)
        require(not self.undefined, f'{path}: undefined symbols {self.undefined}')

    def window(self, machine, entry, start, end):
        require(self.machine == machine and self.entry == entry,
                f'{self.path}: wrong architecture or entry: {self.machine}, {self.entry:#x}')
        require(self.loads, f'{self.path}: no loadable segments')
        for segment in self.loads:
            for key in ('vaddr', 'paddr'):
                address = segment[key]
                require(start <= address < end and address + segment['memory_bytes'] <= end,
                        f'{self.path}: load segment escapes private DDR: {segment}')
        if machine == 8:
            require(self.flags & 0xF000F000 == 0x70001000,
                    f'{self.path}: expected MIPS32r2/o32 ABI')
        else:
            require(self.flags & 0xe == 0, f'{self.path}: expected RISC-V soft-float ilp32 ABI, not RVE')

    def binary_matches(self, binary, base):
        require(binary, f'{self.path}: empty binary')
        extent = 0
        for segment in self.loads:
            if not segment['file_bytes']:
                continue
            start = segment['paddr'] - base
            end = start + segment['file_bytes']
            source = segment['file_offset']
            require(0 <= start < end <= len(binary) and
                    binary[start:end] == self.data[source:source + segment['file_bytes']],
                    f'{self.path}: binary differs from ELF load bytes')
            extent = max(extent, end)
        require(len(binary) == extent, f'{self.path}: binary contains unexpected trailing bytes')


def cache_values(build):
    values = {}
    cache = build / 'CMakeCache.txt'
    if cache.exists():
        for line in cache.read_text(encoding='utf-8').splitlines():
            if '=' in line and ':' in line.split('=', 1)[0]:
                key = line.split(':', 1)[0]
                values[key] = line.split('=', 1)[1]
    return values


def artifact(binary, elf):
    data = binary.read_bytes()
    return {
        'binary': str(binary.resolve()), 'elf': str(elf.path.resolve()),
        'binary_bytes': len(data), 'binary_sha256': hashlib.sha256(data).hexdigest(),
        'binary_crc32': f'{zlib.crc32(data) & 0xffffffff:08x}',
        'entry': f'0x{elf.entry:08x}', 'load_segments': elf.loads,
    }


def embedded_test_media(build, elf, package):
    path = cache_values(build).get('PLAYER_TEST_AVI')
    if not path:
        require('player_test_media_start' not in elf.symbols, 'Unexpected embedded test media')
        return {'enabled': False}
    media = Path(path).read_bytes()
    start = elf.symbols.get('player_test_media_start', 0) - MIPS_BASE
    end = elf.symbols.get('player_test_media_end', 0) - MIPS_BASE
    require(0 <= start < end <= len(package) and package[start:end] == media,
            'Embedded diagnostic AVI differs from source')
    return {'enabled': True, 'path': str(Path(path).resolve()), 'bytes': len(media),
            'offset': start, 'sha256': hashlib.sha256(media).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--riscv-build', type=Path)
    parser.add_argument('--mips-build', type=Path)
    parser.add_argument('--emit-embed-header', type=Path)
    parser.add_argument('--audio-owner', choices=('mips', 'riscv'), default='riscv')
    parser.add_argument('--riscv-clock-hz', type=int, default=576000000)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--mips-only', action='store_true')
    parser.add_argument('--wsl-distribution', help='WSL distribution used for the Linux Victory0 compiler')
    args = parser.parse_args()
    if args.mips_only:
        require(args.mips_build and args.audio_owner == 'mips' and not args.riscv_build,
                'MIPS-only requires --mips-build, --audio-owner mips and no RISC-V build')
        elf = Elf32(args.mips_build / 'halley7m_player.elf')
        elf.window(8, MIPS_BASE, MIPS_BASE, MIPS_END)
        require(elf.symbols.get('__heap_end') == MIPS_END and
                MIPS_BASE <= elf.symbols.get('__heap_start', 0) < MIPS_END,
                'MIPS heap changed')
        require('player_riscv_image_start' not in elf.symbols,
                'MIPS-only firmware must contain no RISC-V payload')
        binary = args.mips_build / 'halley7m_player.bin'
        require(0 < binary.stat().st_size <= MAX_PACKAGE, 'MIPS-only binary exceeds upload bound')
        elf.binary_matches(binary.read_bytes(), MIPS_BASE)
        cache = cache_values(args.mips_build)
        require(cache.get('PLAYER_MIPS_ONLY') == 'ON' and cache.get('PLAYER_AUDIO_OWNER') == 'mips',
                'MIPS-only build options mismatch')
        manifest = {
            'board': 'HALLEY7-M-KIT X2600M 64MiB DDR2',
            'artifact_type': 'player-mips-only-diagnostics', 'audio_owner': 'mips',
            'audio_enabled_at_boot': False, 'audio_service_built': True,
            'audio_default_volume_percent': default_volume(),
            'hardware_validated': False, 'amp_validated': False,
            'mips': artifact(binary, elf), 'riscv': {'status': 'missing'},
            'test_media': embedded_test_media(args.mips_build, elf, binary.read_bytes()),
            'memory': {'mips_base': hex(MIPS_BASE), 'mips_heap_end': hex(MIPS_END),
                       'riscv_reserved': [hex(RISCV_BASE), hex(RISCV_END)],
                       'shared_reserved': [hex(SHARED_BASE), hex(DDR_END)]},
        }
        if args.manifest:
            args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        print(f'MIPS-only verified: {binary.stat().st_size} bytes; RISC-V missing; hardware pending')
        return
    require(args.riscv_build is not None, 'Full AMP verification requires --riscv-build')
    remote_elf = Elf32(args.riscv_build / 'player_riscv.elf')
    remote_elf.window(243, RISCV_ENTRY, RISCV_BASE, RISCV_END)
    for name in ('__image_start', '__image_end', '_bss_start', '_bss_end', '_stack_start', '__private_end'):
        require(name in remote_elf.symbols, f'Missing RISC-V linker symbol: {name}')
    require(remote_elf.symbols['__image_start'] == RISCV_BASE and
            remote_elf.symbols['__private_end'] == RISCV_END and
            remote_elf.symbols['_bss_end'] <= remote_elf.symbols['_stack_start'] <= RISCV_END,
            'RISC-V private window/stack changed')
    remote_binary = args.riscv_build / 'player_riscv.bin'
    remote_data = remote_binary.read_bytes()
    require(0 < len(remote_data) <= RISCV_END - RISCV_BASE, 'RISC-V binary size invalid')
    remote_elf.binary_matches(remote_data, RISCV_BASE)
    remote_crc = zlib.crc32(remote_data) & 0xffffffff
    if args.emit_embed_header:
        args.emit_embed_header.write_text(
            '#ifndef PLAYER_EMBED_H\n#define PLAYER_EMBED_H\n'
            f'#define PLAYER_RISCV_IMAGE_BYTES {len(remote_data)}u\n'
            f'#define PLAYER_RISCV_IMAGE_CRC32 0x{remote_crc:08x}u\n#endif\n', encoding='utf-8')
    if not args.mips_build:
        require(not args.manifest, '--manifest requires both firmware builds')
        print(f'RISC-V verified: {len(remote_data)} bytes; CRC32={remote_crc:08x}; hardware pending')
        return
    host_elf = Elf32(args.mips_build / 'halley7m_player.elf')
    host_elf.window(8, MIPS_BASE, MIPS_BASE, MIPS_END)
    for name in ('__heap_start', '__heap_end', 'player_riscv_image_start', 'player_riscv_image_end'):
        require(name in host_elf.symbols, f'Missing MIPS linker symbol: {name}')
    require(host_elf.symbols['__heap_end'] == MIPS_END and
            host_elf.symbols['__heap_start'] < MIPS_END, 'MIPS heap overlaps AMP reservation')
    host_binary = args.mips_build / 'halley7m_player.bin'
    package = host_binary.read_bytes()
    require(0 < len(package) <= MAX_PACKAGE, 'Compact package exceeds fixed 6 MiB upload window')
    host_elf.binary_matches(package, MIPS_BASE)
    offset = host_elf.symbols['player_riscv_image_start'] - MIPS_BASE
    embedded_end = host_elf.symbols['player_riscv_image_end'] - MIPS_BASE
    require(embedded_end - offset == len(remote_data) and
            package[offset:embedded_end] == remote_data, 'Embedded RISC-V binary differs from verified build')
    for build, core in ((args.riscv_build, 'riscv'), (args.mips_build, 'mips')):
        cache = cache_values(build)
        require(cache.get('PLAYER_CORE') == core, f'{core}: CMake processor mismatch')
        require(cache.get('PLAYER_AUDIO_OWNER') == args.audio_owner, f'{core}: audio owner mismatch')
        require(cache.get('PLAYER_RISCV_CLOCK_HZ') == str(args.riscv_clock_hz), f'{core}: timer clock mismatch')
    manifest = {
        'board': 'HALLEY7-M-KIT X2600M 64MiB DDR2',
        'artifact_type': 'player-amp-audio-diagnostics',
        'protocol_version': source_version('player_amp.h', 'PLAYER_AMP_VERSION'),
        'memory_version': source_version('player_memory.h', 'PLAYER_MEMORY_VERSION'),
        'audio_owner': args.audio_owner, 'audio_enabled_at_boot': False,
        'audio_service_built': True, 'audio_default_volume_percent': default_volume(),
        'riscv_clock_hz_configured': args.riscv_clock_hz,
        'riscv_clock_measured': False, 'hardware_validated': False,
        'mips': artifact(host_binary, host_elf), 'riscv': artifact(remote_binary, remote_elf),
        'embedded_offset': offset, 'embedded_bytes': len(remote_data),
        'test_media': embedded_test_media(args.mips_build, host_elf, package),
        'memory': {'mips_base': hex(MIPS_BASE), 'mips_heap_end': hex(MIPS_END),
                   'riscv_physical_base': hex(RISCV_BASE), 'riscv_private_end': hex(RISCV_END),
                   'shared_physical_base': hex(SHARED_BASE), 'shared_bytes': DDR_END - SHARED_BASE,
                   'shared_mips_uncached': '0xa3e00000', 'pcm_offset': 0x10000, 'pcm_bytes': 256 * 1024},
        'checks': ['ELF32 architecture/entry', 'all load segments inside fixed private windows',
                   'no undefined global symbols', 'MIPS heap below AMP reservation',
                   'embedded image exact-byte match', '6 MiB compact upload bound', 'matching build options'],
    }
    for core, build in (('mips', args.mips_build), ('riscv', args.riscv_build)):
        compiler = cache_values(build).get('CMAKE_C_COMPILER')
        if compiler:
            command = [compiler, '--version']
            if compiler.startswith('/') and args.wsl_distribution:
                command = ['wsl.exe', '-d', args.wsl_distribution, '--exec', *command]
            manifest[core]['compiler'] = subprocess.check_output(command, text=True).splitlines()[0]
    if args.manifest:
        args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Player package verified: {len(package)} bytes; RISC-V offset={offset:#x}; hardware pending')


if __name__ == '__main__':
    main()
