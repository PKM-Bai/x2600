#!/usr/bin/env python3
"""Run selected 30s format fixtures once, with exact PCM counts and fresh evidence.

Only approved 7% volume is sent. No reboot, flash, upload, media retry, or
volume escalation. --dry-run opens no serial port and creates no files.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('player_board_runner', ROOT / 'scripts/run-player-board.py')
BASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASE)
BoardError, value = BASE.BoardError, BASE.value


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(65536), b''):
            digest.update(block)
    return digest.hexdigest()


def path_text(text):
    if not text or any(ord(ch) < 32 or ord(ch) == 127 for ch in text) or len(text.encode('utf-8')) >= 2048:
        raise ValueError('board paths must be nonempty UTF-8 paths shorter than 2048 bytes, without control characters')
    return text.rstrip('/')


def expected_case(entry, audio_root, video_root):
    """Apply the production source/FAAD priming rule on source PCM frames."""
    name = entry['file']
    if not name or '/' in name or '\\' in name or name in ('.', '..'):
        raise ValueError('manifest file must be a basename: ' + name)
    rate, channels, packets = (int(entry[key]) for key in ('source_rate', 'source_channels', 'audio_packets'))
    if rate not in (44100, 48000) or channels not in (1, 2) or packets <= 0:
        raise ValueError('this format matrix requires 44.1/48kHz mono/stereo: ' + name)
    suffix = Path(name).suffix.lower()
    if suffix == '.mp3':
        coded = packets * 1152
        leading = sum(int(row.get('skip_samples', 0)) for row in entry['skip_side_data'])
        trailing = sum(int(row.get('discard_padding', 0)) for row in entry['skip_side_data'])
        if leading <= 0 or trailing < 0:
            raise ValueError('MP3 fixture must declare verified Xing/LAME trim: ' + name)
        frames, kind = coded - leading - trailing, 3
        rule = f'{packets}*1152-{leading}-{trailing}'
    elif suffix == '.aac':
        coded, frames, kind = packets * 1024, (packets - 1) * 1024, 4
        rule = f'{packets}*1024-1024 (FAAD first LC frame)'
    elif suffix in ('.m4a', '.mp4'):
        info = entry['native_demux']['info']
        audio = info['audio']
        if int(audio['rate']) != rate or int(audio['channels']) != channels or int(audio['frame_samples']) != 1024:
            raise ValueError('demux/manifest audio mismatch: ' + name)
        coded, start, end = packets * 1024, 0, int(audio['duration'])
        if int(audio['edit']):
            start = int(audio['media_start'])
            scale = int(info['movie_scale'])
            if start < 0 or scale <= 0 or int(audio['empty_duration']):
                raise ValueError('unsupported fixture edit: ' + name)
            end = start + int(audio['edit_duration']) * rate // scale
            if end > int(audio['duration']):
                if end - int(audio['duration']) > (rate + scale - 1) // scale:
                    raise ValueError('edit exceeds track by more than one movie tick: ' + name)
                end = int(audio['duration'])
        frames, kind = max(0, min(coded, end) - max(1024, start)), 5
        rule = f'intersection([{start},{end}),[1024,{coded}))'
    else:
        raise ValueError('unsupported fixture extension: ' + name)
    declared_coded = int(entry.get('coded_frames_assuming_lc1024_or_mpeg1layer3_1152', coded))
    if coded != declared_coded or frames <= 0:
        raise ValueError('invalid coded/trimmed frame count: ' + name)
    samples = frames * 48000 // rate
    video = int(entry.get('video_frames', 0))
    board_path = path_text((video_root if video else audio_root) + '/' + name)
    return dict(file=name, path=board_path, source_rate=rate, source_channels=channels,
                expected_kind=kind, expected_source_frames=frames, expected_samples=samples,
                expected_video_frames=video, quiet_seconds=samples / 48000 + 3,
                sample_rule=rule + f'; floor(source*48000/{rate})',
                expected_file_sha256=entry.get('sha256'), expected_file_bytes=entry.get('bytes'))


def make_plan(manifest, selected, audio_root, video_root):
    entries = {entry['file']: entry for entry in manifest['files']}
    if len(entries) != len(manifest['files']):
        raise ValueError('duplicate filenames in fixture manifest')
    names = selected or list(entries)
    if not names or len(set(names)) != len(names):
        raise ValueError('--file selections must be distinct; no automatic repeated plays')
    missing = [name for name in names if name not in entries]
    if missing:
        raise ValueError('unknown --file: ' + ', '.join(missing))
    return [expected_case(entries[name], audio_root, video_root) for name in names]


class FormatsRunner(BASE.Runner):
    def __init__(self, console, args, plan, report, progress=None, save=None):
        super().__init__(console, args, progress)
        self.plan, self.report, self.save = plan, report, save or (lambda _: None)
        self.current = None

    def event(self, name, **details):
        super().event(name, **details)
        self.save(self.report)

    def idle(self, ignore_faults=False):
        state, audio = super().idle(ignore_faults)
        if value(state, 'kind') or value(state, 'error') or value(audio, 'state') != 0:
            raise BoardError('IDLE must have kind=0, error=0 and CLOSED audio: ' + str((state, audio)))
        return state, audio

    def quiet(self, seconds):
        deadline = self.console.clock() + seconds
        while self.console.clock() < deadline:
            # No UART queries while the worker is playing; progress is host
            # output only. Every individual receive window is <=30 seconds.
            self.console.collect_until(min(deadline, self.console.clock() + 30))
            if self.console.clock() < deadline:
                self.event('receiving', file=self.current['file'])

    def validate_final(self, case, state, audio, gui):
        self.clean_session(state)
        self.clean_audio(audio)
        self.clean_gui(gui, require_playback=True)
        expected = case['expected_samples']
        if value(state, 'state') != 4 or value(state, 'kind') != case['expected_kind'] or state.get('path') != case['path']:
            raise BoardError('media did not reach the expected ENDED path/kind: ' + str(state))
        if state.get('samples') != f'{expected}/{expected}' or value(state, 'result'):
            raise BoardError('session sample total/request result mismatch: ' + str(state))
        if any(value(audio, key) != expected for key in ('total', 'written', 'completed')):
            raise BoardError(f'audio must seal exactly {expected} valid samples: {audio}')
        if any(value(state['queues'], key) for key in ('video_packets', 'audio_packets', 'pcm_queued')):
            raise BoardError('ENDED still reports queued media: ' + str(state['queues']))
        if value(audio, 'state') != 7 or value(audio, 'clock_valid') != 1:
            raise BoardError('audio must be DRAINED with a valid clock: ' + str(audio))
        decoded, displayed, dropped = (value(state, key) for key in ('decoded', 'displayed', 'dropped'))
        late = value(state['queues'], 'late')
        if min(decoded, displayed, dropped, late) < 0 or decoded != case['expected_video_frames'] or displayed + dropped != decoded:
            raise BoardError('video frame accounting mismatch: ' + str(state))
        return dict(applicable=bool(decoded), decoded=decoded, displayed=displayed, dropped=dropped,
                    late=late, max_late_us=value(state['queues'], 'max_late_us'),
                    passed=not bool(dropped or late), physical_output='not-evaluated')

    def run_case(self, case):
        result = dict(case, status='running', functional_pass=False)
        self.current = result
        self.report['cases'].append(result)
        self.event('case-start', file=case['file'], expected_samples=case['expected_samples'], path=case['path'])
        self.post('stop')
        state, audio = self.idle()
        result['before'] = dict(session=state, audio=audio, heap=self.query('heap', 'HEAP'))
        self.post('volume 7')
        self.post('play ' + case['path'])
        self.event('playing', file=case['file'], quiet_seconds=case['quiet_seconds'])
        self.quiet(case['quiet_seconds'])
        # Large status dumps follow EOF, so they cannot create playback drops.
        result['ended'] = {}
        for key, command, marker in (('session', 'session', 'SESSION_STATUS'), ('audio', 'audio', 'AUDIO_STATUS'),
                                     ('gui', 'gui', 'GUI_STATUS')):
            result['ended'][key] = self.query(command, marker)
        ended = result['ended']
        if value(ended['session'], 'session') == value(state, 'session'):
            raise BoardError('PLAY never created a fresh media session')
        result['video_realtime'] = self.validate_final(case, ended['session'], ended['audio'], ended['gui'])
        ended['storage'] = self.query('session storage', 'STORAGE_SEEK')
        if value(ended['storage'], 'result'):
            raise BoardError('storage status failed: ' + str(ended['storage']))
        if case['expected_video_frames']:
            ended['display'] = self.query('display', 'DISPLAY_ROUTE')
            route = ended['display']
            if value(route, 'error') or value(route, 'rot_errors') or value(route, 'rotated') != 1 or value(route, 'width') != 1280 or value(route, 'height') != 720:
                raise BoardError('landscape display route failed: ' + str(route))
            self.console.collect_until(self.console.clock() + 0.3)
        ended['heap'] = self.query('heap', 'HEAP')
        if value(ended['heap'], 'media_stack_free_words') <= 0:
            raise BoardError('media task has no measured stack headroom')
        self.post('stop')
        state, audio = self.idle()
        result['after'] = dict(session=state, audio=audio, heap=self.query('heap', 'HEAP'))
        result['heap_free_delta'] = value(result['after']['heap'], 'free') - value(result['before']['heap'], 'free')
        result['functional_pass'] = True
        result['status'] = 'passed' if result['video_realtime']['passed'] else 'passed-with-video-issues'
        self.event('case-complete', file=case['file'], status=result['status'], samples=case['expected_samples'],
                   video_realtime_pass=result['video_realtime']['passed'], heap_free_delta=result['heap_free_delta'])

    def cleanup(self):
        self.event('cleanup-start')
        cleanup = dict(idle_confirmed=False)
        if self.current is not None:
            self.current['cleanup'] = cleanup
        try:
            record = self.query('stop', 'SESSION_POST', timeout=3, ignore_faults=True)
            cleanup['stop'] = record
            if value(record, 'result'):
                raise BoardError('cleanup STOP was rejected')
            state, audio = self.idle(ignore_faults=True)
            cleanup.update(idle_confirmed=True, session=state, audio=audio)
        except Exception as error:
            cleanup['error'] = str(error)
            self.console.host('cleanup STOP/IDLE not confirmed: ' + str(error))
        self.event('cleanup-complete', idle_confirmed=cleanup['idle_confirmed'])

    def run(self):
        self.report['status'] = 'running'
        self.event('batch-start', selected=len(self.plan))
        try:
            for case in self.plan:
                self.run_case(case)
        except (Exception, KeyboardInterrupt) as error:
            if self.current is not None:
                self.current.update(status='failed', error=str(error) or 'KeyboardInterrupt')
            self.report.update(status='failed', passed=False, error=str(error) or 'KeyboardInterrupt')
            self.event('failed', file=self.current['file'] if self.current else None, error=self.report['error'])
            self.cleanup()
            raise
        realtime = all(case['video_realtime']['passed'] for case in self.report['cases'])
        self.report.update(status='complete' if realtime else 'complete-with-video-issues', collection_complete=True,
                           functional_pass=True, video_realtime_pass=realtime, passed=realtime)
        self.event('batch-complete', functional_pass=True, video_realtime_pass=realtime, cases=len(self.plan))


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=ROOT / 'build/player-fixture/formats/manifest.json')
    parser.add_argument('--root', default='0:/multimedia', help='contains audio/ and video/ subdirectories')
    parser.add_argument('--audio-root', help='override the exact audio fixture directory on TF')
    parser.add_argument('--video-root', help='override the exact video fixture directory on TF')
    parser.add_argument('--file', action='append', default=[], help='repeat with exact manifest basenames for a subset')
    parser.add_argument('--port')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--log', type=Path, help='fresh raw UART log; never append/overwrite')
    parser.add_argument('--json', type=Path, dest='json_path', help='fresh report; defaults to log with .json suffix')
    parser.add_argument('--firmware-manifest', type=Path, help='record expected host artifact only; no board hash verification')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    try:
        args.audio_root = path_text(args.audio_root or path_text(args.root) + '/audio')
        args.video_root = path_text(args.video_root or path_text(args.root) + '/video')
        manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
        args.plan = make_plan(manifest, args.file, args.audio_root, args.video_root)
        if args.baud <= 0:
            raise ValueError('--baud must be positive')
        if not args.dry_run and (not args.port or not args.log):
            raise ValueError('--port and --log are required for a board run')
        if args.log:
            args.json_path = args.json_path or args.log.with_suffix('.json')
            if args.log.resolve() == args.json_path.resolve():
                raise ValueError('--log and --json must be different new files')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    return args


def main(argv=None):
    args = arguments(argv)
    if args.dry_run:
        print(json.dumps(dict(volume_percent=7, board_access=False, cases=args.plan), indent=2, ensure_ascii=False))
        return 0
    report = dict(schema=1, started_utc=datetime.now(timezone.utc).isoformat(), status='preparing', passed=False,
                  collection_complete=False, volume_percent=7, fixture_manifest=str(args.manifest.resolve()),
                  fixture_manifest_sha256=sha256(args.manifest), expected_cases=args.plan, cases=[],
                  expected_firmware_artifact=None, board_firmware_identity_verified=False,
                  board_media_sha256_verified=False, physical_audio_video='not-evaluated')
    if args.firmware_manifest:
        report['expected_firmware_artifact'] = dict(path=str(args.firmware_manifest.resolve()),
            manifest_sha256=sha256(args.firmware_manifest), manifest=json.loads(args.firmware_manifest.read_text(encoding='utf-8')),
            scope='expected host artifact, not a board-queried firmware hash')
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.json_path.parent.mkdir(parents=True, exist_ok=True)
    if args.log.exists() or args.json_path.exists():
        print('Refusing to overwrite or append an existing log/report', file=sys.stderr)
        return 2
    log = args.log.open('xb')
    output = args.json_path.open('x', encoding='utf-8')
    def save(current):
        output.seek(0)
        json.dump(current, output, indent=2, ensure_ascii=False)
        output.write('\n'); output.truncate(); output.flush()
    save(report)
    uart = console = runner = None
    code = 1
    try:
        import serial
        uart = serial.Serial(port=None, baudrate=args.baud, timeout=0.1, write_timeout=2)
        uart.dtr = uart.rts = False
        uart.port = args.port
        uart.open()
        console = BASE.Console(uart, log)
        runner = FormatsRunner(console, args, args.plan, report, save=save)
        console.host('expected artifact is host evidence only; board SHA was not queried')
        console.collect_until(console.clock() + 0.2)
        runner.run()
        code = 0 if report['passed'] else 1
    except (Exception, KeyboardInterrupt) as error:
        report.update(status='failed', passed=False, error=str(error) or 'KeyboardInterrupt')
        if runner is None:
            print('Board run failed before commands: ' + str(error), file=sys.stderr)
        code = 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        if console:
            console.close_log()
        if uart is not None:
            uart.close()
        log.close()
        report['raw_log'] = dict(path=str(args.log.resolve()), sha256=sha256(args.log))
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save(report)
        output.close()
    return code


if __name__ == '__main__':
    sys.exit(main())
