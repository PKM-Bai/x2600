"""Run an idle player's decoder-only CRC probe and compare every PC frame.

No firmware upload, reset, Flash programming, DPU takeover or audio output.
The reference must describe the same file selected by --file on the board.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import time
import serial

ROOT = Path(__file__).resolve().parents[1]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM52')
    parser.add_argument('--file', required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    board = module('codec_board_console', 'run-player-board.py')
    checker = module('codec_frame_checker', 'check-player-codec-crc.py')
    if not args.reference.is_file():
        parser.error('Reference does not exist')
    report_path = args.log.with_suffix('.json')
    if args.log.exists() or report_path.exists():
        parser.error('Use fresh log/report paths')
    uart = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2,
                         xonxoff=False, rtscts=False, dsrdtr=False)
    uart.dtr = uart.rts = False
    uart.port = args.port
    with args.log.open('xb') as log:
        uart.open()
        console = board.Console(uart, log)
        try:
            console.collect_until(time.monotonic() + 0.2)
            state, _ = console.command('session', 'SESSION_STATUS')
            if int(state['state']) or int(state['pinned']):
                raise board.BoardError('Stop the session before decoder verification')
            audio, _ = console.command('audio', 'AUDIO_STATUS')
            if int(audio['active']):
                raise board.BoardError('Audio must be inactive')
            # The initial allocation/geometry line also lets an early failure
            # finish immediately, including failures before the first frame.
            _, text = console.command('video verify ' + args.file, 'VIDEO_PROBE', timeout=60)
            received = text.encode('utf-8')
            deadline = time.monotonic() + args.timeout
            count, printed = 0, 0
            while not re.search(rb'\bVIDEO_PROBE result=[^\r\n]*\r?\n', received):
                if time.monotonic() >= deadline:
                    raise board.BoardError('Decoder verification timed out')
                received += console.receive()
                count = received.count(b'VIDEO_FRAME_CRC ')
                if count // 30 > printed:
                    printed = count // 30
                    print(f'Decoder CRC: {count} frames received', flush=True)
            print(re.search(rb'\bVIDEO_PROBE result=[^\r\n]*', received)[0].decode(), flush=True)
            for command, marker in [('heap', 'HEAP'), ('status', 'AMP_STATUS'), ('gui', 'GUI_STATUS')]:
                _, text = console.command(command, marker)
                print(text, end='', flush=True)
        finally:
            console.close_log()
            uart.close()
    report = checker.check(args.log, args.reference)
    report['board_file'] = args.file
    with report_path.open('x', encoding='utf-8') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(f'PASS: {report["frames"]} decoded frames match both PC NV12 planes; {report_path}', flush=True)


if __name__ == '__main__':
    main()
