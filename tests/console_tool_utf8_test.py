"""Offline CLI boundary check: serial constructors are replaced before main()."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / 'build/regba-host/console-tools'
OUTPUT.mkdir(parents=True, exist_ok=True)


def module(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts' / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class BeforeSerial(Exception):
    pass


def check_cli(value, option, argument, accepted):
    diagnostics = io.StringIO()
    with patch.object(sys, 'argv', [value.__name__, option, argument]), \
            patch('serial.Serial', side_effect=BeforeSerial) as constructor, \
            contextlib.redirect_stderr(diagnostics):
        try:
            value.main()
        except BeforeSerial:
            assert accepted, (value.__name__, repr(argument), 'invalid input reached serial')
            assert constructor.call_count == 1
        except SystemExit as error:
            assert not accepted and error.code == 2, (value.__name__, error.code, diagnostics.getvalue())
            assert not constructor.called, 'validation opened serial before rejecting the input'
        else:
            raise AssertionError('CLI unexpectedly returned')


rom_cases = [
    ('Chinese path', '/GBA/ROMS/龙珠大冒险.gba', True),
    ('ASCII 500 bytes', '/' + 'a' * 495 + '.gba', True),
    ('Chinese 500 bytes', '/' + '汉' * 165 + '.gba', True),
    ('ASCII 501 bytes', '/' + 'a' * 496 + '.gba', False),
    ('Chinese 501 bytes', '/' + '汉' * 165 + 'x.gba', False),
    ('empty', '', False), ('whitespace only', '  ', False),
    ('CR', '/GBA/test.gba\rstatus', False),
    ('LF', '/GBA/test.gba\nstatus', False),
    ('invalid UTF-8 surrogate', '/GBA/\ud800.gba', False),
]
result = {'serial_opened': False, 'rom_cases': [], 'command_cases': []}
for name in ('soak-regba', 'test-audio-lifecycle', 'profile-regba'):
    value = module(name)
    for label, argument, accepted in rom_cases:
        check_cli(value, '--rom', argument, accepted)
        result['rom_cases'].append({'tool': name, 'case': label, 'accepted': accepted})

board = module('board-console')
for label, argument, accepted in [
    ('Chinese command', 'load /GBA/ROMS/龙珠大冒险.gba', True),
    ('empty', '', False), ('whitespace only', '  ', False),
    ('CR', 'status\rreset', False), ('LF', 'status\nreset', False),
    ('invalid UTF-8 surrogate', 'load /GBA/\ud800.gba', False),
]:
    check_cli(board, '--command', argument, accepted)
    result['command_cases'].append({'case': label, 'accepted': accepted})


class FakeSerial:
    def __init__(self):
        self.writes = []
        self.in_waiting = 0
    def write(self, data):
        self.writes.append(data)
    def flush(self):
        pass
    def open(self):
        pass
    def read(self, count):
        return b''
    def __enter__(self):
        return self
    def __exit__(self, *args):
        pass


soak = module('soak-regba')
fake = FakeSerial()
raw = io.BytesIO()
console = soak.Console(fake, raw)
command = 'load /GBA/ROMS/龙珠大冒险.gba'
console.send(command)
assert fake.writes == [command.encode('utf-8') + b'\r']
for invalid in ('', ' ', 'status\rreset', 'status\nreset', '\ud800'):
    previous = (len(fake.writes), raw.getvalue())
    try:
        console.send(invalid)
    except (ValueError, UnicodeEncodeError):
        pass
    else:
        raise AssertionError('invalid console line was sent')
    assert previous == (len(fake.writes), raw.getvalue())

fake = FakeSerial()
clock = iter(range(1000))
log = OUTPUT / 'board-console-utf8-offline.log'
log.write_bytes(b'')
with patch.object(sys, 'argv', ['board-console', '--command', command, '--duration', '0.01', '--log', str(log)]), \
        patch('serial.Serial', return_value=fake), \
        patch.object(board.time, 'monotonic', side_effect=lambda: next(clock)):
    board.main()
assert fake.writes == [command.encode('utf-8') + b'\r']
result['console_send_utf8_pass'] = True
result['board_console_send_utf8_pass'] = True
result['pass'] = True
output = OUTPUT / 'validation.json'
output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(f'PASS: 30 ROM + 6 command validation cases; UTF-8 sends; no physical serial access; {output}')
