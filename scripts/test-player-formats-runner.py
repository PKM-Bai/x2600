#!/usr/bin/env python3
"""Test the real formats runner/Console using fragmented fake UART and virtual time."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('formats_runner', ROOT / 'scripts/run-player-formats-board.py')
RUN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN)
MANIFEST_PATH = ROOT / 'tests/player_formats_manifest_fixture.json'
MANIFEST = json.loads(MANIFEST_PATH.read_text(encoding='utf-8'))
PLAN = RUN.make_plan(MANIFEST, [], '0:/multimedia/audio', '0:/multimedia/video')


class FakeBoard:
    def __init__(self, plan, fault=None):
        self.now, self.output, self.commands = 0.0, bytearray(), []
        self.plan = {case['path']: case for case in plan}
        self.case = None
        self.started = None
        self.stopped, self.request, self.session, self.volume = True, 0, 0, 7
        self.fault, self.fault_emitted = fault, False
        self.dtr = self.rts = True
        self.port, self.opened, self.closed = None, False, False
        self.input = bytearray()

    def pause(self, seconds):
        self.now += seconds

    def clock(self):
        return self.now

    @property
    def in_waiting(self):
        return min(97, len(self.output))

    def read(self, count):
        if self.fault == 'serial' and self.started is not None and self.now - self.started > 3:
            raise OSError('fake UART disconnected')
        if self.output:
            count = min(count, 13)  # Split markers, records and UTF-8.
            data = bytes(self.output[:count]); del self.output[:count]
            self.now += 0.0001
            return data
        self.now += 0.05
        if self.fault == 'async' and self.started is not None and self.now - self.started > 3 and not self.fault_emitted:
            self.output.extend(b'AMP_FORCE_SILENCE reason=fake-fault\r\n')
            self.fault_emitted = True
        return b''

    def flush(self):
        pass

    def open(self):
        assert self.dtr is False and self.rts is False
        self.opened = True

    def close(self):
        self.closed = True

    def ended(self):
        if self.started is None:
            return False
        extra = 10 if self.fault == 'late-eof' else 0
        return self.now - self.started >= self.case['expected_samples'] / 48000 + extra

    def write(self, payload):
        self.input.extend(payload)
        while b'\r' in self.input:
            end = self.input.index(b'\r') + 1
            command = bytes(self.input[:end]); del self.input[:end]
            self.write_command(command)
        return len(payload)

    def write_command(self, payload):
        if self.fault == 'serial' and self.started is not None and self.now - self.started > 3:
            raise OSError('fake UART disconnected')
        command = payload.decode('utf-8').strip()
        self.commands.append((self.now, command))
        response = ''
        if command == 'stop' or command.startswith(('play ', 'volume ')):
            self.request += 1
            result = -16 if command.startswith('play ') and self.fault == 'reject' else 0
            if command == 'stop':
                if self.fault != 'stop-fails' or self.started is None:
                    self.stopped = True
            elif command.startswith('volume '):
                assert command == 'volume 7'
                self.volume = 7
            elif not result:
                self.case = self.plan[command[5:]]
                self.started, self.stopped = self.now, False
                if self.fault != 'stale-session':
                    self.session += 1
            kind = 5 if command == 'stop' else 6 if command.startswith('volume ') else 1
            response = f'SESSION_POST request={self.request} kind={kind} result={result}'
        elif command == 'session':
            ended = self.ended()
            state = 0 if self.stopped else 4 if ended else 2
            kind = 0 if self.stopped else self.case['expected_kind']
            expected = self.case['expected_samples'] if self.case else 0
            samples = expected if ended else 0
            video = self.case['expected_video_frames'] if self.case and ended else 0
            dropped = int(self.fault == 'video-issues' and video > 0)
            late = dropped
            path = self.case['path'] if self.case else ''
            if self.fault == 'wrong-path' and not self.stopped:
                path += '-wrong'
            pinned = int(self.fault == 'stop-fails' and not self.stopped)
            response = (f'SESSION_STATUS session={self.session} state={state} kind={kind} samples={samples}/{expected} '
                        f'decoded={video} displayed={video-dropped} dropped={dropped} underruns=0 volume={self.volume} '
                        f'pinned={pinned} error=0 request={self.request} result=0 path={path} detail=\r\n'
                        f'SESSION_QUEUES late={late} max_late_us=26666 video_packets=0 audio_packets=0 pcm_queued=0')
        elif command == 'audio':
            total = self.case['expected_samples'] if self.case else 0
            samples = total if self.ended() else 0
            if self.fault == 'wrong-samples' and not self.stopped:
                samples -= 1
            volume = 10 if self.fault == 'volume' and not self.stopped else self.volume
            state = 0 if self.stopped else 7 if self.ended() else 3
            error = int(self.fault == 'dma-error' and not self.stopped)
            queue_error = int(self.fault == 'queue-error' and not self.stopped)
            response = (f'AUDIO_STATUS session={self.session} state={state} active={int(not self.stopped)} total={total} '
                        f'written={samples} completed={samples} underruns=0 volume={volume} clock_valid={int(not self.stopped)} '
                        f'error=0 dma_errors={error} dma_timeouts=0 queue_underruns={queue_error} fifo_underruns=0')
        elif command == 'gui':
            response = 'GUI_STATUS page=1 menu=0 generation=3 rendered=100 presented=100 dropped_repeats=0 dropped_actions=0 error=0 stack_free_words=3000'
        elif command == 'heap':
            headroom = 0 if self.fault == 'stack' and not self.stopped else 11000
            response = f'HEAP free=30000000 isr_unused=1024 media_stack_free_words={headroom}'
        elif command == 'session storage':
            response = 'STORAGE_SEEK result=0 enabled=0 capacity=0 required=0 used=0 build_us=0 seeks=10 max_seek_us=5'
        elif command == 'display':
            response = ('DISPLAY_ROUTE width=1280 height=720 rotated=1 generation=3 phase=0 error=0 writebacks=1000 '
                        'rot_sof=1000 rot_eof=1000 repeats=10 rot_errors=0\r\nDISPLAY clocks=1 init=1 running=1 backlight=1 panel_result=0 pending=0 scanout=1')
        else:
            response = 'Commands: unknown command'
        self.output.extend(('player> ' + command + '\r\n' + response + '\r\n').encode('utf-8'))
        return len(payload)


def exercise(plan=None, fault=None):
    plan = plan or [PLAN[0]]
    board, log, events = FakeBoard(plan, fault), io.BytesIO(), []
    report = {'cases': [], 'passed': False, 'collection_complete': False}
    console = RUN.BASE.Console(board, log, board.clock, board.pause)
    runner = RUN.FormatsRunner(console, SimpleNamespace(), plan, report, events.append)
    error = None
    try:
        runner.run()
    except (RUN.BoardError, OSError) as caught:
        error = caught
    console.close_log()
    return board, log.getvalue().decode('utf-8'), report, events, error


class FormatsTests(unittest.TestCase):
    def test_actual_manifest_exact_14_counts_and_paths(self):
        self.assertEqual(len(PLAN), 14)
        for case in PLAN:
            rate = case['source_rate']
            expected = 1440008 if case['file'].endswith('.aac') and rate == 44100 else 1440768 if case['file'].endswith('.aac') else 1440000
            self.assertEqual(case['expected_samples'], expected)
            self.assertEqual(case['expected_video_frames'], 900 if case['file'].endswith('.mp4') else 0)
            self.assertIn('/video/' if case['expected_video_frames'] else '/audio/', case['path'])

    def test_subset_roots_unknown_and_duplicate(self):
        names = [PLAN[-1]['file'], PLAN[0]['file']]
        plan = RUN.make_plan(MANIFEST, names, '0:/音频', '0:/视频')
        self.assertEqual([row['file'] for row in plan], names)
        self.assertTrue(plan[0]['path'].startswith('0:/视频/'))
        self.assertTrue(plan[1]['path'].startswith('0:/音频/'))
        with self.assertRaises(ValueError):
            RUN.make_plan(MANIFEST, ['missing.mp3'], '0:/a', '0:/v')
        with self.assertRaises(ValueError):
            RUN.make_plan(MANIFEST, names * 2, '0:/a', '0:/v')

    def test_all_real_manifest_cases_complete_with_fragmented_uart(self):
        board, log, report, events, error = exercise(PLAN)
        self.assertIsNone(error, str(error))
        self.assertTrue(report['passed'])
        self.assertEqual(len(report['cases']), 14)
        commands = [cmd for _, cmd in board.commands]
        self.assertEqual(sum(cmd.startswith('play ') for cmd in commands), 14)
        self.assertEqual(sum(cmd == 'volume 7' for cmd in commands), 14)
        self.assertEqual(commands[:6], ['stop', 'session', 'audio', 'heap', 'volume 7', 'play ' + PLAN[0]['path']])
        self.assertEqual(commands[-4:], ['stop', 'session', 'audio', 'heap'])
        self.assertFalse(any(cmd in ('reboot', 'tone', 'amp start', 'amp stop') for cmd in commands))
        for index, (when, command) in enumerate(board.commands):
            if command.startswith('play '):
                case = next(row for row in PLAN if row['path'] == command[5:])
                self.assertGreaterEqual(board.commands[index + 1][0] - when, case['quiet_seconds'])
                self.assertEqual(board.commands[index + 1][1], 'session')
        self.assertIn('AUDIO_STATUS', log)
        self.assertEqual(events[-1]['event'], 'batch-complete')
        timestamps = [event['host_elapsed_s'] for event in events]
        self.assertLess(max(b - a for a, b in zip(timestamps, timestamps[1:])), 60)

    def test_counts_fresh_session_path_clock_volume_and_error_failures_stop_batch(self):
        for fault in ('wrong-samples', 'stale-session', 'wrong-path', 'volume', 'dma-error', 'queue-error', 'stack', 'late-eof', 'reject'):
            with self.subTest(fault=fault):
                board, _, report, _, error = exercise(PLAN[:2], fault)
                self.assertIsNotNone(error)
                self.assertEqual(report['status'], 'failed')
                self.assertFalse(report['passed'])
                self.assertEqual(len(report['cases']), 1)
                self.assertTrue(report['cases'][0]['cleanup']['idle_confirmed'])
                self.assertEqual(sum(cmd.startswith('play ') for _, cmd in board.commands), 1)
                self.assertTrue(board.stopped)
                self.assertFalse(any(cmd.startswith('volume ') and cmd != 'volume 7' for _, cmd in board.commands))

    def test_video_issue_is_separate_and_does_not_hide_functional_results(self):
        board, _, report, _, error = exercise([PLAN[-1], PLAN[0]], 'video-issues')
        self.assertIsNone(error)
        self.assertTrue(report['functional_pass'])
        self.assertTrue(report['collection_complete'])
        self.assertFalse(report['video_realtime_pass'])
        self.assertFalse(report['passed'])
        self.assertEqual(report['status'], 'complete-with-video-issues')
        self.assertEqual(report['cases'][0]['status'], 'passed-with-video-issues')
        self.assertEqual(report['cases'][0]['video_realtime']['dropped'], 1)
        self.assertEqual(sum(cmd.startswith('play ') for _, cmd in board.commands), 2)

    def test_async_fault_and_disconnect_keep_failed_report_and_best_effort_stop(self):
        for fault in ('async', 'serial'):
            with self.subTest(fault=fault):
                board, log, report, _, error = exercise(fault=fault)
                self.assertIsNotNone(error)
                self.assertFalse(report['passed'])
                self.assertIn('cleanup', report['cases'][0])
                self.assertEqual(report['cases'][0]['cleanup']['idle_confirmed'], fault != 'serial')
                self.assertEqual(sum(cmd.startswith('play ') for _, cmd in board.commands), 1)
                if fault == 'async':
                    self.assertIn('AMP_FORCE_SILENCE', log)

    def test_stop_failure_is_not_released_or_replayed(self):
        board, _, report, _, error = exercise(PLAN[:2], 'stop-fails')
        self.assertIsNotNone(error)
        self.assertFalse(report['cases'][0]['functional_pass'])
        self.assertFalse(report['cases'][0]['cleanup']['idle_confirmed'])
        self.assertEqual(sum(cmd.startswith('play ') for _, cmd in board.commands), 1)

    def test_dry_run_uses_no_serial_or_output_files(self):
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/run-player-formats-board.py'), '--dry-run',
                                 '--manifest', str(MANIFEST_PATH), '--file', PLAN[1]['file']], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertFalse(output['board_access'])
        self.assertEqual(output['volume_percent'], 7)
        self.assertEqual(output['cases'][0]['expected_samples'], 1440008)

    def test_main_persists_fresh_json_raw_log_and_expected_artifact_scope(self):
        workspace = ROOT / 'build/player-host/formats-runner-tests'
        workspace.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workspace) as directory:
            folder = Path(directory)
            self.assertTrue(folder.resolve().is_relative_to(workspace.resolve()))
            log, report = folder / 'test.log', folder / 'test.json'
            artifact = folder / 'expected.json'
            artifact.write_text(json.dumps({'mips': {'sha256': 'a' * 64}}))
            board = FakeBoard([PLAN[0]])
            factory = mock.Mock(return_value=board)
            serial = SimpleNamespace(Serial=factory)
            argv = ['--manifest', str(MANIFEST_PATH), '--port', 'FAKE', '--log', str(log), '--json', str(report), '--file', PLAN[0]['file'],
                    '--firmware-manifest', str(artifact)]
            # Main uses the real Console and its clock default; replace only
            # the clock binding, keeping production parsing and runner logic.
            actual_console = RUN.BASE.Console
            def console(uart, output):
                return actual_console(uart, output, board.clock, board.pause)
            with mock.patch.dict(sys.modules, {'serial': serial}), mock.patch.object(RUN.BASE, 'Console', console), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(RUN.main(argv), 0)
            saved = json.loads(report.read_text(encoding='utf-8'))
            self.assertTrue(saved['passed'])
            self.assertFalse(saved['board_firmware_identity_verified'])
            self.assertEqual(saved['raw_log']['sha256'], RUN.sha256(log))
            self.assertIn('not a board-queried', saved['expected_firmware_artifact']['scope'])
            self.assertTrue(board.opened and board.closed)
            self.assertFalse(board.dtr or board.rts)
            original_log, original_report = log.read_bytes(), report.read_bytes()
            factory.reset_mock()
            with mock.patch.dict(sys.modules, {'serial': serial}), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(RUN.main(argv), 2)
            factory.assert_not_called()
            self.assertEqual(log.read_bytes(), original_log)
            self.assertEqual(report.read_bytes(), original_report)

    def test_main_failure_persists_stop_result_and_uart_log(self):
        workspace = ROOT / 'build/player-host/formats-runner-tests'
        workspace.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workspace) as directory:
            folder = Path(directory)
            self.assertTrue(folder.resolve().is_relative_to(workspace.resolve()))
            board = FakeBoard([PLAN[1]], 'wrong-samples')
            serial = SimpleNamespace(Serial=mock.Mock(return_value=board))
            actual_console = RUN.BASE.Console
            def console(uart, output):
                return actual_console(uart, output, board.clock, board.pause)
            log = folder / 'failed.log'
            with mock.patch.dict(sys.modules, {'serial': serial}), mock.patch.object(RUN.BASE, 'Console', console), contextlib.redirect_stdout(io.StringIO()):
                code = RUN.main(['--manifest', str(MANIFEST_PATH), '--port', 'FAKE', '--log', str(log), '--file', PLAN[1]['file']])
            saved = json.loads(log.with_suffix('.json').read_text(encoding='utf-8'))
            self.assertEqual(code, 1)
            self.assertEqual(saved['status'], 'failed')
            self.assertFalse(saved['passed'])
            self.assertTrue(saved['cases'][0]['cleanup']['idle_confirmed'])
            self.assertEqual(saved['raw_log']['sha256'], RUN.sha256(log))
            self.assertTrue(board.closed)


if __name__ == '__main__':
    unittest.main(verbosity=2)
