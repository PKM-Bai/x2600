"""Geometry runner tests: dry-run, real sample protocol, and reused virtual UART."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import uuid
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUN = load('geometry_runner', ROOT / 'scripts/run-player-geometry-board.py')
FAKE = load('formats_uart_tests', ROOT / 'scripts/test-player-formats-runner.py')
MANIFESTS = [json.loads(path.read_text(encoding='utf-8')) for path in (
    ROOT / 'build/player-fixture/geometry-color/manifest.json',
    ROOT / 'build/player-fixture/geometry-level-tags/manifest.json')]
PLAN = RUN.make_plan(MANIFESTS, [], '0:/multimedia/video')


def display_text(case):
    width, height = case['canvas']
    f = case['format']
    return (f'DISPLAY_ROUTE width={width} height={height} rotated={case["rotated"]} phase=0 error=0 rot_errors=0\r\n'
            f'DISPLAY_FORMAT coded={f["coded"]} visible={f["visible"]} crop={f["crop"]} stride={f["stride"]} '
            f'sar={f["sar"]} viewport={",".join(map(str, f["viewport"]))} matrix={f["matrix"]} full={f["full"]} '
            'matrix_inferred=0 range_inferred=0\r\n'
            'ROT_EVENT index=0 status=0\r\nDISPLAY_END result=0\r\n')


class GeometryBoard(FAKE.FakeBoard):
    def write_command(self, payload):
        if payload.decode().strip() == 'display':
            self.commands.append((self.now, 'display'))
            case = self.case or next(iter(self.plan.values()))
            self.output.extend(display_text(case).encode())
            return len(payload)
        return super().write_command(payload)


class GeometryRunnerTest(unittest.TestCase):
    def test_thirteen_and_padding_sar_native(self):
        self.assertEqual(len(PLAN), 13)
        cases = {case['file']: case for case in PLAN}
        self.assertEqual(cases['geometry-720x1280-601-full.mp4']['canvas'], [720, 1280])
        self.assertEqual(cases['geometry-854x480-601-full.mp4']['format']['coded'], '864x480')
        self.assertEqual(cases['geometry-720x576-sar16x15-601-limited.mp4']['format']['viewport'], [160, 0, 960, 720])
        self.assertTrue(all(case['expected_samples'] == 144000 for case in PLAN))

    def test_display_metadata_and_native(self):
        for case in PLAN:
            text = display_text(case)
            route, fmt = (RUN.response_record(text, marker) for marker in ('DISPLAY_ROUTE', 'DISPLAY_FORMAT'))
            RUN.validate_display(case, route, fmt)
            wrong = dict(fmt, full=str(1 - case['format']['full']))
            with self.assertRaises(RUN.BoardError):
                RUN.validate_display(case, route, wrong)

    def test_dry_run_no_console_or_files(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            target = ROOT / 'build' / ('geometry-dry-' + uuid.uuid4().hex) / 'raw.log'
            with mock.patch.object(RUN.BASE, 'Console', side_effect=AssertionError('UART must not open')):
                self.assertEqual(RUN.main(['--dry-run', '--log', str(target)]), 0)
            data = json.loads(output.getvalue())
            self.assertEqual(len(data['cases']), 13)
            self.assertFalse(data['board_access'])
            self.assertFalse(target.parent.exists())

    def test_real_sample_schema_tolerance_and_completeness(self):
        points = [{'x': 80, 'y': 180, 'expected_rgb': [10, 20, 30]}]
        good = 'DISPLAY_SAMPLES model=0 count=1 result=0\r\nDISPLAY_PIXEL index=0 x=80 y=180 argb=ff0c141e\r\n'
        self.assertEqual(RUN.check_samples(good, points)[0]['maximum_error'], 2)
        with self.assertRaises(RUN.BoardError):
            RUN.check_samples(good.replace('ff0c141e', 'ff0d141e'), points)
        with self.assertRaises(RUN.BoardError):
            RUN.check_samples(good.rstrip('\r\n'), points)

    def test_fragmented_uart_wide_native_no_active_queries(self):
        for case in (PLAN[0], next(case for case in PLAN if case['rotated'] == 0)):
            board, log = GeometryBoard([case]), io.BytesIO()
            console = RUN.BASE.Console(board, log, board.clock, board.pause)
            args = SimpleNamespace(check_csc=False, level_manifest=ROOT / 'build/player-fixture/geometry-level-tags/manifest.json')
            report = {'cases': []}
            runner = RUN.GeometryRunner(console, args, [case], report, lambda event: None)
            runner.run()
            self.assertTrue(report['functional_pass'])
            self.assertEqual(report['cases'][0]['heap_recovery']['status'], 'new-ui-baseline')
            started = next(time for time, command in board.commands if command.startswith('play '))
            self.assertFalse([(time, command) for time, command in board.commands
                              if started < time < started + case['quiet_seconds']])

    def test_drop_is_separate_from_functional_accounting(self):
        case = PLAN[0]
        board, log = GeometryBoard([case], fault='video-issues'), io.BytesIO()
        console = RUN.BASE.Console(board, log, board.clock, board.pause)
        args = SimpleNamespace(check_csc=False, level_manifest=ROOT / 'build/player-fixture/geometry-level-tags/manifest.json')
        report = {'cases': []}
        runner = RUN.GeometryRunner(console, args, [case], report, lambda event: None)
        runner.run()
        self.assertTrue(report['functional_pass'])
        self.assertFalse(report['video_realtime_pass'])
        self.assertEqual(report['cases'][0]['video_realtime']['dropped'], 1)

    def test_retained_title_heap_delta_and_same_ui_leak(self):
        class TitleBoard(GeometryBoard):
            leak = 0

            def write_command(self, payload):
                if payload.decode().strip() == 'heap':
                    self.commands.append((self.now, 'heap'))
                    free = 30000000 - len(self.case['file']) * 8 - self.leak
                    self.output.extend(f'HEAP free={free} media_stack_free_words=11000\r\n'.encode())
                    return len(payload)
                return super().write_command(payload)

        case = PLAN[0]
        short = dict(case, file='test.mp4', path='0:/multimedia/video/test.mp4')
        board, log = TitleBoard([case, short]), io.BytesIO()
        board.case = short
        console = RUN.BASE.Console(board, log, board.clock, board.pause)
        args = SimpleNamespace(check_csc=False, level_manifest=ROOT / 'build/player-fixture/geometry-level-tags/manifest.json')
        report = {'cases': []}
        runner = RUN.GeometryRunner(console, args, [case], report, lambda event: None)
        runner.run_case(case)
        self.assertLess(report['cases'][-1]['heap_free_delta'], 0)
        self.assertEqual(report['cases'][-1]['heap_recovery']['status'], 'new-ui-baseline')
        runner.run_case(case)
        self.assertEqual(report['cases'][-1]['heap_recovery']['status'], 'recovered')
        board.leak = 16
        with self.assertRaisesRegex(RUN.BoardError, 'same canvas and retained filename'):
            runner.run_case(case)
        self.assertEqual(report['cases'][-1]['heap_recovery']['status'], 'failed')
        self.assertEqual(report['cases'][-1]['heap_recovery']['reference_free'] -
                         report['cases'][-1]['heap_recovery']['after_free'], 16)


if __name__ == '__main__':
    unittest.main()
