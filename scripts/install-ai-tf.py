"""Install prepared AI assets into a new /ai directory on an explicitly named TF root."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def install(card, resources, key_file):
    card = card.resolve(strict=True)
    target = card / 'ai'
    if target.exists():
        raise ValueError('Destination ai directory already exists; no files changed')
    source = resources / 'ai'
    manifest = json.loads((source / 'SOURCE.json').read_text('utf-8'))
    files = {name: (source / name).read_bytes() for name in ('font.aif', 'pinyin.aip', 'SOURCE.json', 'config.example.ini')}
    for name in ('font.aif', 'pinyin.aip'):
        expected = manifest['assets'][name]
        if len(files[name]) != expected['bytes'] or hashlib.sha256(files[name]).hexdigest() != expected['sha256']:
            raise ValueError('Source asset mismatch: ' + name)
    for license_file in (source / 'licenses').glob('*.txt'):
        files['licenses/' + license_file.name] = license_file.read_bytes()
    if key_file:
        spec = importlib.util.spec_from_file_location('ai_deepseek_probe', ROOT / 'scripts/ai-deepseek-probe.py')
        probe = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(probe)
        key = probe.credentials(key_file)
        files['config.ini'] = files['config.example.ini'].replace(b'api_key=REPLACE_ME', ('api_key=' + key).encode('ascii'))
    # Exclusive creation prevents replacing existing card content or configuration.
    target.mkdir()
    for name, data in files.items():
        path = target / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if path.read_bytes() != data:
            raise IOError('TF readback mismatch: ' + name)
    result = {'target': str(target), 'files_verified': len(files),
              'assets': manifest['assets'], 'configuration_present': bool(key_file)}
    print(json.dumps(result))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--card', type=Path, required=True)
    parser.add_argument('--resources', type=Path, default=ROOT / 'build/ai-resources')
    parser.add_argument('--key-file', type=Path)
    args = parser.parse_args()
    install(args.card, args.resources, args.key_file)
