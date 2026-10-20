#!/usr/bin/env python3
"""Replay the thirteen 3s geometry/colour fixtures at 7%; dry-run never opens UART."""
import argparse
from fractions import Fraction
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('geometry_formats_base', ROOT / 'scripts/run-player-formats-board.py')
FORMATS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FORMATS)
BASE, BoardError, value = FORMATS.BASE, FORMATS.BoardError, FORMATS.value
CSC = {(6, 1): (0x400, 0x59c, 0x160, 0x2db, 0x717, 0),
       (6, 0): (0x4a8, 0x662, 0x191, 0x340, 0x812, 16),
       (1, 1): (0x400, 0x64d, 0xc0, 0x1df, 0x76c, 0),
       (1, 0): (0x4a8, 0x72c, 0xda, 0x222, 0x873, 16)}


def expected_case(entry, root):
    name = entry['file']
    if not name or Path(name).name != name or '/' in name or '\\' in name:
        raise ValueError('fixture file must be a basename')
    w, h, sn, sd = (int(entry[key]) for key in ('visible_width', 'visible_height', 'sar_num', 'sar_den'))
    if min(w, h, sn, sd) <= 0 or int(entry['decoded_frames']) != 90 or int(entry['container_audio_frames']) != 144000:
        raise ValueError('geometry fixture must contain90 frames/144000 samples with a positive SAR')
    dar = Fraction(w * sn, h * sd)
    cw, ch = (1280, 720) if dar >= 1 else (720, 1280)
    if dar * ch >= cw:
        vw, vh = cw, (cw * dar.denominator // dar.numerator) & ~1
    else:
        vw, vh = (ch * dar.numerator // dar.denominator) & ~1, ch
    matrix, full = int(entry['matrix_coefficients']), int(entry['full_range'])
    y, rv, gu, gv, bu, sub = CSC[(matrix, full)]
    return dict(file=name, path=FORMATS.path_text(root + '/' + name), expected_kind=5,
                expected_samples=144000, expected_video_frames=90, quiet_seconds=6,
                expected_file_sha256=entry['sha256'], expected_file_bytes=int(entry['bytes']),
                canvas=[cw, ch], rotated=int(cw == 1280),
                format=dict(coded=f"{entry['coded_width']}x{entry['coded_height']}", visible=f'{w}x{h}',
                            crop=f"{entry['crop_left']},{entry['crop_top']}", stride=int(entry['board_stride']),
                            sar=f'{sn}/{sd}', viewport=[(cw-vw)//2, (ch-vh)//2, vw, vh], matrix=matrix, full=full),
                expected_csc_q10=dict(yrv=y | rv << 16, gugv=gu | gv << 16, bu=bu, sub_yuv=sub | 128 << 16))


def make_plan(manifests, selected, root):
    entries = {}
    for manifest in manifests:
        for entry in manifest['files']:
            if entry['file'] in entries:
                raise ValueError('duplicate fixture filename: ' + entry['file'])
            entry = dict(entry)
            # The old verified retag manifest spells its explicit input as
            # top-level source; newer generators write reference_file per item.
            reference = entry.get('reference_file', entry.get('source_file'))
            if reference is None:
                reference = manifest.get('reference_file', manifest.get('source_file', manifest.get('source')))
            verified_retag = (int(entry.get('level_idc', 0)) in (41, 42) and
                              all(entry.get(key) is True for key in
                                  ('vcl_payload_identical', 'non_sps_nals_identical', 'aac_au_and_asc_identical')))
            entry['reference_file'] = (str(reference).replace('\\', '/').rsplit('/', 1)[-1]
                                       if verified_retag and reference else None)
            entries[entry['file']] = entry
    names = selected or list(entries)
    if not names or len(set(names)) != len(names):
        raise ValueError('--file selections must be distinct')
    plan = [expected_case(entries[name], root) for name in names]
    for case in plan:
        if entries[case['file']].get('reference_file'):
            case['reference_file'] = entries[case['file']]['reference_file']
    return plan


def response_record(text, marker):
    rows = re.findall(r'\b' + re.escape(marker) + r' ([^\r\n]*)\r?\n', text)
    if len(rows) != 1:
        raise BoardError('expected one complete ' + marker + ' line')
    return BASE.fields(rows[0])


def validate_display(case, route, actual):
    for key in ('error', 'phase', 'rot_errors'):
        if value(route, key):
            raise BoardError('display route is not clean: ' + str(route))
    if [value(route, 'width'), value(route, 'height')] != case['canvas'] or value(route, 'rotated') != case['rotated']:
        raise BoardError('canvas/orientation mismatch: ' + str(route))
    expected = case['format']
    for key in ('coded', 'visible', 'crop', 'sar'):
        if actual.get(key) != expected[key]:
            raise BoardError('DISPLAY_FORMAT ' + key + ' mismatch: ' + str(actual))
    for key in ('stride', 'matrix', 'full'):
        if value(actual, key) != expected[key]:
            raise BoardError('DISPLAY_FORMAT ' + key + ' mismatch: ' + str(actual))
    if actual.get('viewport') != ','.join(map(str, expected['viewport'])):
        raise BoardError('viewport mismatch: ' + str(actual))
    if value(actual, 'matrix_inferred') or value(actual, 'range_inferred'):
        raise BoardError('controlled SPS colours must not be inferred')


def check_samples(text, points):
    """Parse complete per-pixel records; compare RGB24, ignoring alpha."""
    rows = re.findall(r'\bDISPLAY_PIXEL ([^\r\n]*)\r?\n', text)
    actual = {}
    for index, line in enumerate(rows):
        record = BASE.fields(line)
        if value(record, 'index') != index:
            raise BoardError('display pixel indices are incomplete or unordered')
        position = value(record, 'x'), value(record, 'y')
        if position in actual:
            raise BoardError('duplicate display sample position')
        pixel = record.get('argb', record.get('rgb', ''))
        actual[position] = int(pixel, 16)
    if len(actual) != len(points):
        raise BoardError('display sample count mismatch')
    differences = []
    for point in points:
        position = int(point['x']), int(point['y'])
        if position not in actual:
            raise BoardError('missing sample: ' + str(position))
        rgb = [(actual[position] >> shift) & 255 for shift in (16, 8, 0)]
        error = max(abs(a-b) for a, b in zip(rgb, point['expected_rgb']))
        if error > 2:
            raise BoardError(f'CSC pixel {position}: actual={rgb}, expected={point["expected_rgb"]}, tolerance=2')
        differences.append(dict(x=position[0], y=position[1], actual_rgb=rgb, maximum_error=error))
    return differences


class GeometryRunner(FORMATS.FormatsRunner):
    def __init__(self, console, args, plan, report, progress=None, save=None):
        super().__init__(console, args, plan, report, progress, save)
        self.heap_reference = {}
        self.samples = {}
        if args.check_csc:
            self.samples = {entry['file']: entry for entry in json.loads(args.csc_expected.read_text(encoding='utf-8'))['files']}
        report.update(level_manifest=str(args.level_manifest.resolve()),
                      level_manifest_sha256=FORMATS.sha256(args.level_manifest),
                      csc_check_requested=args.check_csc, csc_scope='metadata-only' if not args.check_csc else 'writeback-pixels; native has no writeback')

    def display(self):
        # DISPLAY_FORMAT precedes ROT_EVENT diagnostics; wait for the explicit
        # end marker before sending a long CSC command into the board RX FIFO.
        _, text = self.console.command('display', 'DISPLAY_END')
        return dict(route=response_record(text, 'DISPLAY_ROUTE'),
                    format=response_record(text, 'DISPLAY_FORMAT'))

    def run_case(self, case):
        result = dict(case, status='running', functional_pass=False)
        self.current = result
        self.report['cases'].append(result)
        self.event('case-start', file=case['file'], path=case['path'])
        self.post('stop')
        state, audio = self.idle()
        before_display = self.display()
        before_heap = self.query('heap', 'HEAP')
        before_route = tuple(value(before_display['route'], key) for key in ('width', 'height'))
        before_title = state.get('path', '').replace('\\', '/').rsplit('/', 1)[-1]
        if before_title:
            before_key = (before_route, before_title)
            self.heap_reference[before_key] = max(self.heap_reference.get(before_key, 0), value(before_heap, 'free'))
        result['before'] = dict(session=state, audio=audio, heap=before_heap, display=before_display)
        self.post('volume 7')
        self.post('play ' + case['path'])
        self.event('playing', file=case['file'], quiet_seconds=case['quiet_seconds'])
        self.quiet(case['quiet_seconds'])  # Receive only; no active UART query during playback.
        ended = result['ended'] = {}
        for key, command, marker in (('session', 'session', 'SESSION_STATUS'), ('audio', 'audio', 'AUDIO_STATUS'), ('gui', 'gui', 'GUI_STATUS')):
            ended[key] = self.query(command, marker)
        if value(ended['session'], 'session') == value(state, 'session'):
            raise BoardError('PLAY did not create a fresh media session')
        result['video_realtime'] = self.validate_final(case, ended['session'], ended['audio'], ended['gui'])
        ended['display'] = self.display()
        validate_display(case, ended['display']['route'], ended['display']['format'])
        result['csc'] = dict(status='metadata-only', expected_registers=case['expected_csc_q10'])
        if self.args.check_csc:
            name = case['file'] if case['file'] in self.samples else case.get('reference_file')
            if not name or name not in self.samples:
                raise BoardError('missing explicit CSC expectation/reference for ' + case['file'])
            sample = self.samples[name]
            points = sample.get('points', [])
            if points:
                if len(points) > 16:
                    raise BoardError('CSC batch exceeds sixteen points')
                command = 'display sample ' + self.args.csc_model + ' ' + ' '.join(str(point[key]) for point in points for key in ('x', 'y'))
                for attempt in range(5):
                    done, text = self.console.command(command, 'DISPLAY_SAMPLES')
                    if value(done, 'result') != -11:
                        break
                    self.console.collect_until(self.console.clock() + 0.2) # ENDED-only retry of the query, never replay.
                if value(done, 'result') or value(done, 'count') != len(points) or value(done, 'model') != int(self.args.csc_model):
                    raise BoardError('CSC batch failed: ' + str(done))
                deadline = self.console.clock() + 5
                while len(re.findall(r'\bDISPLAY_PIXEL [^\r\n]*\r?\n', text)) < len(points):
                    if self.console.clock() >= deadline:
                        raise BoardError('timeout waiting for complete CSC pixel batch')
                    text += self.console.receive().decode('utf-8', errors='replace')
                result['csc'] = dict(status='passed', model=self.args.csc_model, points=check_samples(text, points))
            else:
                result['csc'] = dict(status='not-applicable-native-no-writeback')
        ended['heap'] = self.query('heap', 'HEAP')
        if value(ended['heap'], 'media_stack_free_words') <= 0:
            raise BoardError('media task has no stack headroom')
        self.post('stop')
        state, audio = self.idle()
        after_heap = self.query('heap', 'HEAP')
        result['after'] = dict(session=state, audio=audio, heap=after_heap)
        route = tuple(case['canvas'])
        title = state.get('path', '').replace('\\', '/').rsplit('/', 1)[-1]
        key = (route, title)
        reference = self.heap_reference.get(key) if title else None
        recovered = reference is None or value(after_heap, 'free') >= reference
        result['heap_free_delta'] = value(after_heap, 'free') - value(before_heap, 'free')
        # STOP retains the playback filename in LVGL; compare the same title
        # and canvas while always preserving the cross-case heap delta.
        result['heap_recovery'] = dict(route=list(route), retained_filename=title,
                                       reference_free=reference, after_free=value(after_heap, 'free'),
                                       status='recovered' if reference is not None and recovered else 'new-ui-baseline' if recovered else 'failed')
        if not recovered:
            raise BoardError('STOP did not recover the previous heap for the same canvas and retained filename')
        if title:
            self.heap_reference[key] = max(self.heap_reference.get(key, 0), value(after_heap, 'free'))
        result['functional_pass'] = True
        result['status'] = 'passed' if result['video_realtime']['passed'] else 'passed-with-video-issues'
        self.event('case-complete', file=case['file'], status=result['status'], dropped=result['video_realtime']['dropped'], late=result['video_realtime']['late'])


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=ROOT / 'build/player-fixture/geometry-color/manifest.json')
    parser.add_argument('--level-manifest', type=Path, default=ROOT / 'build/player-fixture/geometry-level-tags/manifest.json')
    parser.add_argument('--root', default='0:/multimedia/video')
    parser.add_argument('--file', action='append', default=[])
    parser.add_argument('--port')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--log', type=Path)
    parser.add_argument('--json', type=Path, dest='json_path')
    parser.add_argument('--firmware-manifest', type=Path)
    parser.add_argument('--check-csc', action='store_true')
    parser.add_argument('--csc-expected', type=Path, default=ROOT / 'build/player-fixture/geometry-color/_reports/display-samples.json')
    parser.add_argument('--csc-model', choices=('0', '1'), default='0', help='0: board-verified CUSTOM512 column mapping; 1: raw linear diagnostic')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    try:
        manifests = [json.loads(path.read_text(encoding='utf-8')) for path in (args.manifest, args.level_manifest)]
        args.plan = make_plan(manifests, args.file, FORMATS.path_text(args.root))
        if args.baud <= 0 or not re.fullmatch(r'[A-Za-z0-9_-]+', args.csc_model):
            raise ValueError('invalid baud or CSC model')
        if args.check_csc:
            json.loads(args.csc_expected.read_text(encoding='utf-8'))
        if not args.dry_run and (not args.port or not args.log):
            raise ValueError('--port and --log are required outside dry-run')
        if args.log:
            args.json_path = args.json_path or args.log.with_suffix('.json')
            if args.json_path.resolve() == args.log.resolve():
                raise ValueError('log and report must be distinct fresh paths')
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    return args


def main(argv=None):
    args = arguments(argv)
    # Reuse the existing connection/report lifecycle, with this runner's own
    # plan and orientation-aware checks. Serial is imported only for real runs.
    FORMATS.arguments = lambda argv=None: args
    FORMATS.FormatsRunner = GeometryRunner
    return FORMATS.main(argv)


if __name__ == '__main__':
    sys.exit(main())
