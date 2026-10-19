#!/usr/bin/env python3
"""Meaningful synthetic log regression for the production board analyzer."""
import csv
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('board', ROOT / 'scripts/analyze-player-board.py')
BOARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOARD)


def row(ms, **changes):
    value = dict(label='amp', elapsed_ms=ms, session=1, audio_session=1, amp_session=7,
                 flags=14, state=2, total_samples=86400000, max_late_us=1000,
                 shared_ring_samples=4800, pending_valid_samples=7200,
                 video_packets=8, audio_packets=2, queued_pcm_samples=3200,
                 pinned=0, session_error=0, audio_error=0)
    for name in BOARD.COUNTER_SCOPES:
        value[name] = 0
    value.update(elapsed_samples=ms * 48, decoded_frames=ms * 30 // 1000,
                 displayed_frames=ms * 30 // 1000, audio_written_samples=ms * 48 + 7200,
                 audio_completed_samples=ms * 48)
    for core in ('mips', 'rv'):
        value.update({core + '_valid': 1, core + '_counter_end': 10000000 + ms * 1000,
                      core + '_counter_hz': 1000000, core + '_window_us': 1000000,
                      core + '_busy_bp': 1000 if core == 'mips' else 2000,
                      core + '_heap_free': 1000000, core + '_heap_min': 900000,
                      core + '_heap_total': 2000000, core + '_heap_min_kind': 2 if core == 'mips' else 1})
    value.update(changes)
    return value


def log(rows, summary=None, suffix=''):
    output = io.StringIO(newline='')
    writer = csv.DictWriter(output, fieldnames=list(rows[0]), quoting=csv.QUOTE_NONNUMERIC)
    writer.writeheader()
    writer.writerows(rows)
    text = output.getvalue()
    # Production header is unquoted; its data label is always quoted.
    text = text.replace(','.join('"' + key + '"' for key in rows[0]), ','.join(rows[0]), 1)
    if summary:
        text += summary + '\n'
    return 'UART boot and prompt\n' + text + suffix


def analyze(text, window=600):
    parsed = BOARD.parse_log(text)
    assert len(parsed['runs']) == 1, parsed
    return BOARD.analyze_run(parsed['runs'][0], parsed['statuses'], window, True)


class BoardTests(unittest.TestCase):
    def test_real_duration_weight_not_row_average(self):
        result = analyze(log([row(1000, mips_busy_bp=1000),
                              row(4000, flags=15, mips_window_us=3000000,
                                  mips_busy_bp=3000, rv_window_us=3000000)]), 4)
        self.assertEqual(result['cpu']['mips']['weighted_busy_bp'], 2500)
        self.assertEqual(result['cpu']['mips']['weight_us'], 4000000)
        self.assertEqual(result['counters']['elapsed_samples']['observed_delta'], 144000)
        self.assertTrue(result['counters']['elapsed_samples']['baseline_unknown'])
        self.assertFalse(result['sampling']['csv_window_complete'])  # explicit sampling gap

    def test_duplicates_quoted_label_and_mixed_prefix(self):
        rows = [row(1000, label='AMP,"quiet"'), row(2000, label='AMP,"quiet"', flags=15)]
        text = log(rows) + log(rows)
        text = text.replace('"AMP,""quiet""",1000', '[12:34:56] "AMP,""quiet""",1000')
        result = analyze(text, 2)
        self.assertEqual(result['label'], 'AMP,"quiet"')
        self.assertEqual(result['sampling']['duplicate_rows'], 2)
        self.assertEqual(result['cpu']['mips']['valid_windows'], 2)
        self.assertTrue(result['sampling']['csv_window_complete'])

    def test_stale_rv_not_running_is_missing_not_zero(self):
        rows = [row(1000), row(2000, flags=1, mips_valid=0, rv_valid=0)]
        result = analyze(log(rows), 2)
        self.assertEqual(result['cpu']['mips']['valid_windows'], 1)
        self.assertEqual(result['cpu']['rv']['weighted_busy_bp'], 2000)
        rows = [row(1000, flags=3, rv_valid=0)]
        result = analyze(log(rows), 1)
        self.assertIsNone(result['cpu']['rv']['weighted_busy_bp'])

    def test_duplicate_or_overlapping_cpu_window_is_not_reweighted(self):
        rows = [row(1000), row(2000, mips_counter_end=11000000),
                row(3000, flags=15, mips_window_us=3000000)]
        result = analyze(log(rows), 3)
        self.assertEqual(result['cpu']['mips']['valid_windows'], 1)
        self.assertEqual(result['cpu']['mips']['duplicate_windows'], 1)
        self.assertEqual(result['cpu']['mips']['invalid_windows'], 1)

    def test_scope_change_does_not_add_uint32(self):
        rows = [row(1000, diag_tx=100), row(2000, flags=31, session=2,
                audio_session=2, amp_session=8, elapsed_samples=10,
                decoded_frames=1, diag_tx=2)]
        result = analyze(log(rows), 2)
        self.assertEqual(result['counters']['elapsed_samples']['observed_delta'], 0)
        self.assertEqual(result['counters']['diag_tx']['scope_changes'], 1)
        self.assertEqual(result['scopes']['session'], [1, 2])

    def test_small_wrap_and_same_scope_reset(self):
        rows = [row(1000, diag_tx=0xfffffffe), row(2000, flags=47, diag_tx=3)]
        result = analyze(log(rows), 2)
        self.assertEqual(result['counters']['diag_tx']['observed_delta'], 5)
        self.assertEqual(result['counters']['diag_tx']['wraps'], 1)
        rows = [row(1000, diag_tx=100), row(2000, flags=47, diag_tx=0)]
        result = analyze(log(rows), 2)
        self.assertEqual(result['counters']['diag_tx']['reset_candidates'], 1)
        self.assertEqual(result['counters']['diag_tx']['observed_delta'], 0)

    def test_truncated_missing_and_conflicting_csv(self):
        rows = [row(1000), row(2000, flags=15)]
        summary = 'MEASURE_SUMMARY label=amp active=0 records=3/1860 elapsed_ms=3000 reason=manual'
        text = log(rows, summary) + '"amp",3000,1,1\n'
        result = analyze(text, 3)
        self.assertEqual(result['sampling']['missing_dump_rows'], 1)
        self.assertEqual(result['sampling']['malformed_rows'], 1)
        self.assertFalse(result['sampling']['csv_window_complete'])
        result = analyze(log(rows) + log([row(1000, elapsed_samples=999)]), 2)
        self.assertEqual(result['sampling']['conflicting_elapsed_ms'], [1000])

    def test_window_does_not_consume_later_rows(self):
        rows = [row(1000), row(2000), row(3000, flags=15, mips_busy_bp=9000)]
        result = analyze(log(rows), 2)
        self.assertEqual(result['sampling']['rows'], 2)
        self.assertEqual(result['cpu']['mips']['weighted_busy_bp'], 1000)
        self.assertTrue(result['sampling']['csv_window_complete'])
        # A huge gap straddling the endpoint cannot look like full coverage.
        result = analyze(log([row(1000), row(900000, flags=15)]), 600)
        self.assertFalse(result['sampling']['csv_window_complete'])

    def test_invalid_flags_and_negative_errors_remain_visible(self):
        result = analyze(log([row(1000), row(2000, flags=15 | 64 | 128 | 256,
            shared_ring_samples=123456, pending_valid_samples=999999, session_error=-5)]), 2)
        self.assertEqual(result['watermarks']['shared_ring_samples']['max'], 4800)
        self.assertEqual(result['watermarks']['pending_valid_samples']['max'], 7200)
        self.assertEqual(result['error_observations']['session_error']['min'], -5)
        self.assertEqual(result['error_observations']['session_error']['nonzero_observations'], 1)

    def test_false_eof_or_filename_is_not_full_run(self):
        summary = ('MEASURE_SUMMARY label=amp active=0 reason=eof records=1/1860 '
            'elapsed_ms=1800000 target_s=1800 samples=948480 decoded=594 displayed=593 '
            'dropped=0 late=0 underruns=0 dma_errors=0 dma_timeouts=0')
        result = analyze(log([row(1000, flags=15)], summary))
        self.assertEqual(result['completion']['internal_30min'], 'not-proven')
        self.assertFalse(result['sampling']['window_reached'])

    def test_real_ended_counts_prove_only_internal_completion(self):
        end = row(1800100, flags=15, state=4, elapsed_samples=86400000,
            decoded_frames=54000, displayed_frames=54000,
            audio_written_samples=86400000, audio_completed_samples=86400000)
        result = analyze(log([end]))
        self.assertEqual(result['completion']['internal_30min'], 'evidenced')
        self.assertTrue(result['sampling']['window_reached'])  # full log extends beyond 600s
        self.assertFalse(result['sampling']['csv_window_complete'])  # one row is no CPU window coverage
        self.assertEqual(result['completion']['physical_sync_100ms'], 'not-evaluated')
        end['displayed_frames'] = 53999
        self.assertEqual(analyze(log([end]))['completion']['internal_30min'], 'not-proven')

    def test_error_after_comparison_window_cannot_be_hidden(self):
        rows = [row(1000), row(700000, dma_errors=1),
            row(1800100, flags=47, state=4, elapsed_samples=86400000,
                decoded_frames=54000, displayed_frames=54000,
                audio_written_samples=86400000, audio_completed_samples=86400000)]
        result = analyze(log(rows))
        self.assertEqual(result['completion']['internal_30min'], 'not-proven')
        self.assertIn('CSV曾记录音频/协议错误或pinned', result['completion']['issues'])

    def test_occasional_drop_preserves_internal_completion_and_records_deltas(self):
        rows = [row(1000), row(2000, dropped_frames=1, displayed_frames=59, late_frames=2),
            row(1800100, flags=15, state=4, elapsed_samples=86400000,
                decoded_frames=54000, displayed_frames=53999, dropped_frames=1, late_frames=2,
                audio_written_samples=86400000, audio_completed_samples=86400000)]
        result = analyze(log(rows))['completion']
        self.assertEqual(result['internal_30min'], 'evidenced')
        self.assertEqual(result['video_acceptance'], 'needs-review')
        facts = result['video']['sample_interval_deltas']
        self.assertEqual((facts[0]['from_ms'], facts[0]['to_ms']), (1000, 2000))
        self.assertEqual((facts[0]['dropped_delta'], facts[0]['late_delta']), (1, 2))
        self.assertEqual((facts[1]['dropped_delta'], facts[1]['late_delta']), (0, 0))

    def test_sustained_drop_has_no_invented_automatic_threshold(self):
        rows = [row(1000), row(1800100, flags=15, state=4, elapsed_samples=86400000,
                decoded_frames=54000, displayed_frames=50000, dropped_frames=4000,
                audio_written_samples=86400000, audio_completed_samples=86400000)]
        result = analyze(log(rows))['completion']
        self.assertEqual(result['internal_30min'], 'evidenced')
        self.assertEqual(result['video_acceptance'], 'needs-review')
        self.assertEqual(result['video']['observed_cumulative_peaks']['dropped'], 4000)

    def test_ended_status_with_drop_requires_conservation_and_clean_audio_summary(self):
        summary = ('MEASURE_SUMMARY label=amp active=0 reason=eof records=1800/1860 '
            'elapsed_ms=1800000 target_s=1800 samples=86400000 decoded=54000 displayed=53999 '
            'dropped=1 late=1 underruns=0 dma_errors=0 dma_timeouts=0')
        status = ('SESSION_STATUS session=1 state=4 kind=2 samples=86400000/86400000 '
            'decoded=54000 displayed=53999 dropped=1 underruns=0 pinned=0 error=0\n'
            'SESSION_QUEUES late=1 max_late_us=40000\n')
        result = analyze(log([row(1000, flags=15)], summary, status))['completion']
        self.assertEqual(result['internal_30min'], 'evidenced')
        self.assertEqual(result['video_acceptance'], 'needs-review')
        bad_summary = summary.replace('dma_errors=0', 'dma_errors=1')
        self.assertEqual(analyze(log([row(1000, flags=15)], bad_summary, status))['completion']['internal_30min'], 'not-proven')

    def test_unrelated_ended_session_cannot_match_summary(self):
        summary = ('MEASURE_SUMMARY label=amp active=0 reason=eof records=1800/1860 '
            'elapsed_ms=1800000 target_s=1800 samples=86400000 decoded=54000 displayed=54000 '
            'dropped=0 late=0 underruns=0 dma_errors=0 dma_timeouts=0')
        unrelated = ('SESSION_STATUS session=99 state=4 kind=2 samples=86400000/86400000 '
            'decoded=54000 displayed=54000 dropped=0 underruns=0 pinned=0 error=0\n')
        result = analyze(log([row(1000, flags=15)], summary, unrelated))
        self.assertEqual(result['completion']['internal_30min'], 'not-proven')

    def test_intermediate_final_and_conflicting_summaries_are_ambiguous(self):
        rows = [row(ms, flags=15 if ms in (300000, 600000) else 14)
                for ms in range(1000, 600001, 1000)]
        result = analyze(log(rows))
        self.assertTrue(result['sampling']['intermediate_final'])
        self.assertFalse(result['sampling']['csv_window_complete'])
        summaries = ('MEASURE_SUMMARY label=amp active=0 reason=duration records=300/1860\n'
                     'MEASURE_SUMMARY label=amp active=0 reason=duration records=600/1860\n')
        rows[299]['flags'] = 14
        result = analyze(log(rows, suffix=summaries))
        self.assertTrue(result['sampling']['ambiguous_summary'])
        self.assertFalse(result['sampling']['csv_window_complete'])

    def test_summary_only_real_prefix_and_compare_cli(self):
        prefix = ROOT / 'build/player-tf-avi-prefix-board.log'
        if prefix.is_file():
            result = BOARD.analyze_file(prefix)['runs'][0]
            self.assertEqual(result['source_summaries'][-1]['samples'], '948480')
            self.assertFalse(result['sampling']['csv_window_complete'])
            self.assertEqual(result['completion']['internal_30min'], 'not-proven')
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            paths = [Path(directory) / name for name in ('mips.log', 'amp.log')]
            for path in paths:
                path.write_text(log([row(1000), row(2000, flags=15)]), encoding='utf-8')
            output = Path(directory) / 'comparison.json'
            subprocess.run([sys.executable, str(ROOT / 'scripts/analyze-player-board.py'),
                '--compare', *map(str, paths), '--window', '2', '--output', str(output)], check=True,
                stdout=subprocess.DEVNULL)
            result = json.loads(output.read_text(encoding='utf-8'))
            self.assertEqual(len(result['files']), 2)
            self.assertIn('MIPS忙率', output.with_suffix('.md').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
