"""Generate versioned, read-only AI TF font/dictionary assets; never touch a TF card."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import urllib.request
import zlib

ROOT = Path(__file__).resolve().parents[1]
FONT_SHA = '3d334e48426681bffe5005b4b399b704e8612fc162098d86383fe5463ae95c0e'
DICT_SHA = 'e341598343a0f0f2035bb1aafc34a7f3bb7887deeecb3f60796262aaa2983e6b'
DICT_COMMIT = '0c6861ef7420ee780270ca6d993d18d4101049d0'
DICT_URL = f'https://raw.githubusercontent.com/rime/rime-pinyin-simp/{DICT_COMMIT}/pinyin_simp.dict.yaml'

def asset(magic, count, payload):
    return struct.pack('<4sIII', magic, count, len(payload), zlib.crc32(payload)) + payload

def font_records(raw):
    result = {}
    for line in gzip.decompress(raw).decode('ascii').splitlines():
        code, bits = line.split(':')
        code = int(code, 16)
        if code > 0xffff:
            continue
        if len(bits) not in (32, 64):
            raise ValueError('unsupported glyph width')
        width = len(bits) // 4
        rows = [int(bits[i:i + width // 4], 16) << (16 - width)
                for i in range(0, len(bits), width // 4)]
        result[code] = struct.pack('<HBB16H', code, width, 0, *rows)
    return result

def dictionary_records(raw, glyphs):
    values = {}
    for line in raw.decode('utf-8').splitlines():
        parts = line.split('\t')
        if len(parts) != 3 or not parts[2].isdigit():
            continue
        text, syllables, weight = parts
        key = ''.join(syllables.split()).replace('ü', 'v')
        if not key.isascii() or not key.isalpha() or len(key) > 64 or not 1 <= len(text) <= 4:
            continue
        if any(ord(c) not in glyphs for c in text):
            continue
        try:
            text.encode('gb2312')  # Common simplified repertoire; no unsupported glyph candidates.
        except UnicodeEncodeError:
            continue
        values[key, text] = max(values.get((key, text), 0), int(weight))
    singles = [(k, t, w) for (k, t), w in values.items() if len(t) == 1]
    phrases = sorted(((k, t, w) for (k, t), w in values.items() if len(t) > 1),
                     key=lambda item: (-item[2], item[0], item[1]))[:20000]
    records = sorted(singles + phrases, key=lambda item: (item[0], -item[2], item[1]))
    pool = bytearray(); offsets = {}; index = bytearray()
    def intern(value):
        if value not in offsets:
            offsets[value] = len(records) * 12 + len(pool)
            pool.extend(value.encode('utf-8') + b'\0')
        return offsets[value]
    for key, text, weight in records:
        index.extend(struct.pack('<III', intern(key), intern(text), weight))
    payload = index + pool
    if len(payload) > 2 * 1024 * 1024:
        raise ValueError('dictionary exceeds the 2 MiB budget')
    return records, bytes(payload)

def prepare(output, dictionary):
    source = ROOT / 'third_party/ReGBA/source/tools/unifont.hex.gz'
    font_raw = source.read_bytes(); dictionary_raw = dictionary.read_bytes()
    if hashlib.sha256(font_raw).hexdigest() != FONT_SHA:
        raise ValueError('Unifont source differs from the pinned version')
    if hashlib.sha256(dictionary_raw).hexdigest() != DICT_SHA:
        raise ValueError('pinyin dictionary differs from the pinned source')
    glyphs = font_records(font_raw)
    records, payload = dictionary_records(dictionary_raw, glyphs)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'ai').mkdir(exist_ok=True)
    blobs = {'font.aif': asset(b'AIF1', len(glyphs), b''.join(glyphs[k] for k in sorted(glyphs))),
             'pinyin.aip': asset(b'AIP1', len(records), payload)}
    for name, content in blobs.items():
        (output / 'ai' / name).write_bytes(content)
    # Only the template is copied; an existing populated configuration is never replaced.
    (output / 'ai/config.example.ini').write_bytes((ROOT / 'projects/halley7m-ai/config.example.ini').read_bytes())
    licenses = output / 'ai/licenses'
    licenses.mkdir(exist_ok=True)
    for source_license in (ROOT / 'projects/halley7m-ai/licenses').glob('*.txt'):
        (licenses / source_license.name).write_bytes(source_license.read_bytes())
    needed = set(range(32, 127))
    for path in (ROOT / 'projects/halley7m-ai').rglob('*.c'):
        needed.update(ord(c) for c in path.read_text('utf-8') if 128 <= ord(c) <= 65535)
    missing = needed - glyphs.keys()
    if missing:
        raise ValueError('missing boot/UI font glyphs: ' + repr(sorted(missing)))
    boot = b''.join(glyphs[k] for k in sorted(needed))
    header = ['/* Generated from pinned GNU Unifont; GPL-2.0-or-later. */',
              'static const unsigned char ai_boot_font[] = {']
    raw_boot = asset(b'AIF1', len(needed), boot)
    for start in range(0, len(raw_boot), 24):
        header.append(','.join(str(n) for n in raw_boot[start:start+24]) + ',')
    header += ['};', '']
    (output / 'ai_boot_font.h').write_text('\n'.join(header), encoding='ascii')
    manifest = {'schema': 1, 'font_source_sha256': FONT_SHA, 'dictionary_source_sha256': DICT_SHA,
                'dictionary_url': DICT_URL, 'dictionary_commit': DICT_COMMIT,
                'glyph_count': len(glyphs), 'dictionary_entries': len(records),
                'assets': {n: {'bytes': len(b), 'sha256': hashlib.sha256(b).hexdigest()}
                           for n, b in blobs.items()}}
    (output / 'SOURCE.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'ai/SOURCE.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/ai-resources')
    parser.add_argument('--dictionary', type=Path, default=ROOT / 'build/ai-resources/source/pinyin_simp.dict.yaml')
    parser.add_argument('--download', action='store_true', help='Download the pinned dictionary when absent')
    args = parser.parse_args()
    if not args.dictionary.exists():
        if not args.download:
            parser.error('Pinned dictionary missing; run once with --download or provide --dictionary')
        with urllib.request.urlopen(DICT_URL, timeout=30) as response:
            raw = response.read(2 * 1024 * 1024)
        if hashlib.sha256(raw).hexdigest() != DICT_SHA:
            raise ValueError('Downloaded dictionary SHA256 mismatch')
        args.dictionary.parent.mkdir(parents=True, exist_ok=True)
        args.dictionary.write_bytes(raw)
    prepare(args.output, args.dictionary)

if __name__ == '__main__':
    main()
