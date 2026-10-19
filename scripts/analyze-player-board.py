#!/usr/bin/env python3
"""Analyze player measurement CSV mixed with UART text; never infer optical sync."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
from collections import Counter

FLAGS = {1: 'FINAL', 2: 'MIPS_FRESH', 4: 'RISCV_FRESH', 8: 'RISCV_RUNNING',
         16: 'SCOPE_CHANGED', 32: 'COUNTER_WRAP', 64: 'RING_INVALID',
         128: 'AUDIO_INVALID', 256: 'SNAPSHOT_INVALID'}
SESSION_COUNTERS = ('elapsed_samples', 'decoded_frames', 'displayed_frames',
                    'dropped_frames', 'late_frames', 'audio_underruns')
AUDIO_COUNTERS = ('audio_written_samples', 'audio_completed_samples', 'dma_errors',
                  'dma_timeouts', 'queue_underruns', 'fifo_underruns')
COMM_COUNTERS = ('diag_tx', 'diag_remote_rx', 'diag_event_tx', 'diag_host_rx',
                 'audio_tx', 'audio_remote_rx', 'audio_event_tx', 'audio_host_rx', 'rejected')
COUNTER_SCOPES = {**dict.fromkeys(SESSION_COUNTERS, 'session'),
                  **dict.fromkeys(AUDIO_COUNTERS, 'audio_session'),
                  **dict.fromkeys(COMM_COUNTERS, 'amp_session')}
ERROR_FIELDS = ('audio_underruns', 'dma_errors', 'dma_timeouts', 'queue_underruns',
                'fifo_underruns', 'session_error', 'audio_error', 'pinned', 'rejected')
WATERMARKS = ('shared_ring_samples', 'pending_valid_samples', 'video_packets',
              'audio_packets', 'queued_pcm_samples', 'max_late_us')
BASE_COLUMNS = {'label', 'elapsed_ms', 'session', 'audio_session', 'amp_session', 'flags', 'state'}


def fields(text):
    """UART key=value: values can contain spaces (notably paths and labels)."""
    matches = list(re.finditer(r'(?:^|\s)([A-Za-z_][A-Za-z_0-9]*)=', text))
    return {match[1]: text[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(text)].strip()
            for index, match in enumerate(matches)}


def integer(value):
    return int(value, 10)


def number(mapping, key):
    try:
        return integer(str(mapping[key]))
    except (KeyError, ValueError):
        return None


def parse_log(text):
    """Collect labels independently; repeated dumps are deduplicated later."""
    runs, statuses, header, malformed = {}, [], None, 0

    def run(label):
        return runs.setdefault(label, dict(label=label, rows=[], summaries=[], malformed_rows=0))

    for line_number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip().lstrip('\ufeff')
        if 'MEASURE_SUMMARY ' in line:
            data = fields(line.split('MEASURE_SUMMARY ', 1)[1])
            if data.get('label'):
                data['_line'] = line_number
                run(data['label'])['summaries'].append(data)
            continue
        if 'SESSION_STATUS ' in line:
            status = fields(line.split('SESSION_STATUS ', 1)[1])
            status['_line'] = line_number
            statuses.append(status)
            continue
        if 'SESSION_QUEUES ' in line and statuses:
            statuses[-1]['queues'] = fields(line.split('SESSION_QUEUES ', 1)[1])
            continue
        # Prompts/timestamps before a header are harmless. CSV data retain
        # standard quoted labels (commas and doubled quotes included).
        if 'label,elapsed_ms,session,' in line:
            header = next(csv.reader([line[line.index('label,elapsed_ms,session,'):]]))
            if not BASE_COLUMNS.issubset(header):
                header = None
                malformed += 1
            continue
        if header is None:
            continue
        quoted = re.search(r'"(?:""|[^"])*",', line)
        if quoted:
            line = line[quoted.start():]
        if not line.startswith('"'):
            continue
        try:
            values = next(csv.reader([line], strict=True))
            label = values[0]
            if len(values) != len(header):
                raise ValueError('column count')
            row = dict(zip(header, values))
            row = {key: value if key == 'label' else integer(value) for key, value in row.items()}
            if row['elapsed_ms'] < 0 or row['flags'] < 0:
                raise ValueError('negative timestamp/flags')
            row['_line'] = line_number
            run(label)['rows'].append(row)
        except (csv.Error, ValueError, IndexError):
            malformed += 1
            # Even a partial label can usually identify the damaged dump.
            try:
                run(next(csv.reader([line]))[0])['malformed_rows'] += 1
            except (csv.Error, IndexError):
                pass
    return dict(runs=list(runs.values()), statuses=statuses, malformed_rows=malformed)


def unique_rows(rows):
    unique, duplicates, conflicts = {}, 0, []
    for row in rows:
        timestamp = row['elapsed_ms']
        if timestamp in unique:
            previous = {key: value for key, value in unique[timestamp].items() if key != '_line'}
            current = {key: value for key, value in row.items() if key != '_line'}
            if previous == current:
                duplicates += 1
            else:
                conflicts.append(timestamp)
        else:
            unique[timestamp] = row
    return sorted(unique.values(), key=lambda row: row['elapsed_ms']), duplicates, sorted(set(conflicts))


def cpu_metrics(rows, core):
    fresh = 2 if core == 'mips' else 4
    windows, seen, invalid, duplicate, resets = [], set(), 0, 0, 0
    last = {}
    for row in rows:
        valid = row.get(core + '_valid', 0)
        if not valid:
            if row['flags'] & fresh:
                invalid += 1
            continue
        hz, end = row.get(core + '_counter_hz', 0), row.get(core + '_counter_end', 0)
        duration, busy = row.get(core + '_window_us', 0), row.get(core + '_busy_bp', -1)
        if (not row['flags'] & fresh or (core == 'rv' and not row['flags'] & 8) or
                hz <= 0 or end < 0 or duration < 1000000 or not 0 <= busy <= 10000):
            invalid += 1
            continue
        scope = row['amp_session'] if core == 'rv' else 0
        key = (scope, hz, end)
        if key in seen:
            duplicate += 1
            continue
        seen.add(key)
        group = (scope, hz)
        previous_end = last.get(group)
        if previous_end is not None and end < previous_end:
            resets += 1
        if previous_end is not None and end >= previous_end:
            separation_us = (end - previous_end) * 1000000 / hz
            # Production sampler windows do not overlap. Clock rounding can
            # differ by <1us; duplicated/malformed windows must not bias mean.
            if duration > separation_us + 2:
                invalid += 1
                continue
        last[group] = end
        windows.append((row, duration, busy))
    weight = sum(item[1] for item in windows)
    heap_free = [item[0].get(core + '_heap_free') for item in windows]
    heap_free = [value for value in heap_free if value is not None]
    kinds = {}
    for row, _, _ in windows:
        kind = row.get(core + '_heap_min_kind')
        value = row.get(core + '_heap_min')
        if kind is not None and value is not None:
            kinds[str(kind)] = min(kinds.get(str(kind), value), value)
    return dict(weighted_busy_bp=sum(duration * busy for _, duration, busy in windows) / weight if weight else None,
        valid_windows=len(windows), weight_us=weight, observed_heap_free_min_bytes=min(heap_free) if heap_free else None,
        reported_historical_heap_min_by_kind=kinds, invalid_windows=invalid,
        duplicate_windows=duplicate, counter_resets=resets,
        timing='Select snapshot elapsed_ms in the requested range; weight full source-clock windows. '
               'CPU source windows are not synchronized to measurement boundaries.',
        intervals=[dict(elapsed_ms=row['elapsed_ms'], scope=row['amp_session'] if core == 'rv' else 0,
                        counter_end=row[core + '_counter_end'], counter_hz=row[core + '_counter_hz'],
                        window_us=duration, busy_bp=busy) for row, duration, busy in windows])


def counter_metrics(rows, name, scope):
    samples = [row for row in rows if name in row and not
               (scope == 'session' and row['flags'] & 256)]
    delta = wraps = changes = resets = intervals = 0
    for before, after in zip(samples, samples[1:]):
        if before[scope] != after[scope]:
            changes += 1
            # Missing the old scope's final counter and the new zero baseline:
            # do not join them or silently assert continuous media playback.
            continue
        value = (after[name] - before[name]) & 0xffffffff
        if after[name] < before[name]:
            seconds = max(0.001, (after['elapsed_ms'] - before['elapsed_ms']) / 1000)
            limit = max(1000000, seconds * 1000000)
            if not after['flags'] & 32 or value > limit:
                resets += 1
                continue
            wraps += 1
        delta += value
        intervals += 1
    return dict(observed_delta=delta if samples else None, first_value=samples[0][name] if samples else None,
        last_value=samples[-1][name] if samples else None, intervals=intervals,
        scope_changes=changes, wraps=wraps, reset_candidates=resets, baseline_unknown=True,
        note='Adjacent same-scope differences only; CSV does not export the begin baseline.')


def completion_evidence(rows, summaries, statuses, single_run):
    """Full media completion is separate from a requested CPU sampling window."""
    candidates = []
    for row in rows:
        if row['flags'] & 256:
            continue
        if (row['state'] == 4 and row.get('total_samples') == 86400000 and
                row.get('elapsed_samples') == 86400000 and row.get('decoded_frames') == 54000):
            candidates.append(row)
    sessions = {row['session'] for row in rows}
    statuses = [item for item in statuses if number(item, 'session') in sessions or (single_run and not rows)]
    matching = []
    for status in statuses:
        if (number(status, 'state') == 4 and number(status, 'kind') == 2 and
                status.get('samples') == '86400000/86400000' and number(status, 'decoded') == 54000):
            matching.append(status)
    # Last matching evidence is retained, rather than interpreting reason=eof,
    # target_s=1800 or an acceptance-30min filename as successful playback.
    final = candidates[-1] if candidates else None
    status = matching[-1] if matching else None
    zero_summary = [item for item in summaries if item.get('reason') == 'eof' and
        number(item, 'samples') == 86400000 and number(item, 'decoded') == 54000 and
        number(item, 'displayed') is not None and number(item, 'dropped') is not None and
        number(item, 'displayed') + number(item, 'dropped') == 54000 and
        all(number(item, field) == 0 for field in ('underruns', 'dma_errors', 'dma_timeouts'))]
    issues = []
    if final is None and status is None:
        issues.append('没有完整30分钟AVI的ENDED 54000帧/86400000样本证据')
    if final is not None:
        zero_errors = all(final.get(name) == 0 for name in ERROR_FIELDS)
        clean_frames = (final.get('displayed_frames') is not None and final.get('dropped_frames') is not None and
                        final['displayed_frames'] + final['dropped_frames'] == 54000)
        if not zero_errors:
            issues.append('ENDED CSV的音频/协议错误或pinned非零，或错误字段不全')
        if not clean_frames:
            issues.append('ENDED CSV未证明decoded=displayed+dropped=54000的帧守恒')
    else:
        zero_errors = bool(status and zero_summary and all(number(status, name) == 0 for name in ('underruns', 'pinned', 'error')))
        clean_frames = bool(status and number(status, 'displayed') is not None and
            number(status, 'dropped') is not None and number(status, 'displayed') + number(status, 'dropped') == 54000)
        if status and not zero_errors:
            issues.append('只有ENDED状态，缺少对应完整零错误EOF摘要')
        if status and not clean_frames:
            issues.append('ENDED状态未证明decoded=displayed+dropped=54000的帧守恒')
    if any(any(row.get(name, 0) for name in ERROR_FIELDS) for row in rows):
        issues.append('CSV曾记录音频/协议错误或pinned')
    # This audit uses the complete run, including rows after the CPU comparison
    # cutoff. A reset at minute 20 must not be hidden by a clean first 600s.
    if any(counter_metrics(rows, name, scope)['reset_candidates']
           for name, scope in COUNTER_SCOPES.items()):
        issues.append('完整CSV内存在同scope计数复位疑点')
    ended_scope = final['session'] if final else number(status, 'session') if status else None
    playback = [row for row in rows if row['session'] == ended_scope and row['state'] in (1, 2, 3, 4)]
    if len({row['amp_session'] for row in playback}) > 1:
        issues.append('同一媒体播放期间AMP scope改变，未证明连续运行')
    video = video_evidence(rows, summaries, statuses)
    return dict(internal_30min='evidenced' if not issues else 'not-proven', issues=issues,
        ended_csv_line=final.get('_line') if final else None, ended_session_status=status,
        zero_error_eof_summary=zero_summary[-1] if zero_summary else None,
        video_acceptance=video['assessment'], video=video,
        physical_sync_100ms='not-evaluated',
        note='Board counters prove internal completion only. Calibrated phone sound/light capture is separate.')


def video_evidence(rows, summaries, statuses):
    """Facts for sustained-drop review; no invented pass/fail rate threshold."""
    values = {'dropped': [], 'late': [], 'max_late_us': []}
    for row in rows:
        if not row['flags'] & 256:
            for key, name in (('dropped', 'dropped_frames'), ('late', 'late_frames'), ('max_late_us', 'max_late_us')):
                if name in row:
                    values[key].append(row[name])
    for summary in summaries:
        for key in values:
            value = number(summary, key)
            if value is not None:
                values[key].append(value)
    for status in statuses:
        dropped = number(status, 'dropped')
        if dropped is not None:
            values['dropped'].append(dropped)
        for key in ('late', 'max_late_us'):
            value = number(status.get('queues', {}), key)
            if value is not None:
                values[key].append(value)
    intervals = []
    for before, after in zip(rows, rows[1:]):
        item = dict(from_ms=before['elapsed_ms'], to_ms=after['elapsed_ms'],
            interval_ms=after['elapsed_ms'] - before['elapsed_ms'],
            session=after['session'], state=after['state'],
            scope_changed=before['session'] != after['session'])
        for key, name in (('dropped_delta', 'dropped_frames'), ('late_delta', 'late_frames')):
            metric = counter_metrics([before, after], name, 'session')
            item[key] = metric['observed_delta'] if metric['intervals'] == 1 else None
        intervals.append(item)
    peaks = {key: max(items) if items else None for key, items in values.items()}
    assessment = ('needs-review' if any((peaks[key] or 0) > 0 for key in ('dropped', 'late')) else
                  'no-drop-late-observed' if peaks['dropped'] is not None and peaks['late'] is not None else 'not-evaluated')
    return dict(assessment=assessment, observed_cumulative_peaks=peaks,
        sample_interval_deltas=intervals, first_interval_baseline_unknown=True,
        note='Nonzero drop/late requires review of the actual interval deltas. '
             'Internal completion uses frame conservation; no sustained-drop threshold is imposed.')


def analyze_run(run, statuses, window, single_run):
    all_rows, duplicates, conflicts = unique_rows(run['rows'])
    rows = [row for row in all_rows if row['elapsed_ms'] <= window * 1000]
    summaries = run['summaries']
    summary = summaries[-1] if summaries else None
    completed_summaries = {tuple(sorted((key, value) for key, value in item.items() if key != '_line'))
        for item in summaries if number(item, 'active') == 0}
    ambiguous_summary = len(completed_summaries) > 1
    intermediate_final = any(row['flags'] & 1 for row in all_rows[:-1])
    declared = int(summary['records'].split('/')[0]) if summary and re.fullmatch(r'\d+/\d+', summary.get('records', '')) else None
    gaps = []
    previous_ms = 0
    # Include the first record beyond the requested endpoint to detect a gap
    # crossing that endpoint. Its metrics are not included in the window.
    for row in all_rows:
        if row['elapsed_ms'] - previous_ms > 1500:
            gaps.append(dict(after_ms=previous_ms, before_ms=row['elapsed_ms']))
        previous_ms = row['elapsed_ms']
        if row['elapsed_ms'] >= window * 1000:
            break
    counters = {name: counter_metrics(rows, name, scope) for name, scope in COUNTER_SCOPES.items()}
    cpu = {core: cpu_metrics(rows, core) for core in ('mips', 'rv')}
    flags = {name: sum(bool(row['flags'] & bit) for row in rows) for bit, name in FLAGS.items()}
    watermarks = {}
    for name in WATERMARKS:
        invalid_flag = 64 if name == 'shared_ring_samples' else 128 if name == 'pending_valid_samples' else 256
        values = [row[name] for row in rows if name in row and not row['flags'] & invalid_flag]
        watermarks[name] = dict(min=min(values), max=max(values)) if values else None
    errors = {name: dict(min=min((row[name] for row in rows if name in row), default=None),
                         max=max((row[name] for row in rows if name in row), default=None),
                         nonzero_observations=sum(row.get(name, 0) != 0 for row in rows),
                         observations=sum(name in row for row in rows)) for name in ERROR_FIELDS}
    scopes = {name: list(dict.fromkeys(row[name] for row in rows)) for name in ('session', 'audio_session', 'amp_session')}
    warnings = []
    if not rows:
        warnings.append('无CSV采样；固件摘要仅原样保留，不能重算指定窗口CPU/通信差分')
    if duplicates:
        warnings.append(f'已去重{duplicates}条重复CSV记录')
    if conflicts:
        warnings.append('同标签/时间存在冲突记录，可能重复使用标签或拼接了复位前后的运行')
    if intermediate_final:
        warnings.append('非末行出现FINAL，同标签可能拼接了多次记录器运行')
    if ambiguous_summary:
        warnings.append('同标签有内容不同的已结束摘要，运行边界存在歧义')
    if run['malformed_rows']:
        warnings.append(f'{run["malformed_rows"]}条CSV截断/字段错误')
    if declared is not None and len(all_rows) != declared:
        warnings.append(f'CSV唯一行数{len(all_rows)}与摘要records={declared}不同')
    final_record = bool(all_rows and all_rows[-1]['flags'] & 1)
    if all_rows and not final_record:
        warnings.append('CSV没有FINAL末行，导出可能被截断')
    if gaps:
        warnings.append('存在大于1.5秒的采样间隔；不补造秒采样或把缺少CPU窗口当零负载')
    if any(len(values) > 1 for values in scopes.values()):
        warnings.append('窗口内scope改变，跨scope不拼接计数，不声称一次连续播放')
    if any(item['reset_candidates'] for item in counters.values()):
        warnings.append('发现无法解释为小uint32回绕的计数下降，可能同scope复位/日志拼接')
    if any(flags[name] for name in ('RING_INVALID', 'AUDIO_INVALID', 'SNAPSHOT_INVALID')):
        warnings.append('存在无效快照/水位标志，相关观测不完整')
    if any(value['invalid_windows'] or value['counter_resets'] for value in cpu.values()):
        warnings.append('存在无效/重叠CPU窗口或源计数复位')
    last_ms = rows[-1]['elapsed_ms'] if rows else None
    window_reached = bool(all_rows and all_rows[-1]['elapsed_ms'] >= window * 1000)
    if not window_reached:
        warnings.append(f'没有覆盖完整0..{window:g}秒采样窗口')
    coverage_clean = bool(rows and window_reached and not conflicts and not intermediate_final and
        not ambiguous_summary and not run['malformed_rows'] and
        not gaps and final_record and (declared is None or len(all_rows) == declared))
    complete = completion_evidence(all_rows, summaries, statuses, single_run)
    if conflicts or intermediate_final or ambiguous_summary or run['malformed_rows'] or any(item['reset_candidates'] for item in counters.values()):
        if complete['internal_30min'] == 'evidenced':
            complete['internal_30min'] = 'not-proven'
            complete['issues'].append('日志存在冲突/截断/计数复位/运行边界歧义，需核对原始证据')
    related_statuses = [item for item in statuses if number(item, 'session') in
        {row['session'] for row in all_rows} or (single_run and not all_rows)]
    return dict(label=run['label'], window_s=window, sampling=dict(rows=len(rows), all_unique_rows=len(all_rows),
        duplicate_rows=duplicates, conflicting_elapsed_ms=conflicts, malformed_rows=run['malformed_rows'],
        declared_records=declared, final_record_present=final_record,
        intermediate_final=intermediate_final, ambiguous_summary=ambiguous_summary,
        missing_dump_rows=max(0, declared - len(all_rows)) if declared is not None else None,
        first_ms=rows[0]['elapsed_ms'] if rows else None, last_ms=last_ms,
        gap_intervals=gaps, window_reached=window_reached, csv_window_complete=coverage_clean),
        cpu=cpu, counters=counters, watermarks=watermarks, error_observations=errors,
        flags=flags, scopes=scopes, states=dict(Counter(str(row['state']) for row in rows)),
        source_summaries=summaries, source_session_statuses=related_statuses,
        completion=complete, warnings=warnings)


def analyze_file(path, window=600, label=None):
    path = Path(path)
    raw = path.read_bytes()
    parsed = parse_log(raw.decode('utf-8-sig', errors='replace'))
    runs = parsed['runs']
    if label is not None:
        runs = [run for run in runs if run['label'] == label]
        if not runs:
            raise ValueError(f'日志中没有measurement标签 {label!r}')
    if not runs:
        raise ValueError('没有MEASURE_SUMMARY或measurement CSV')
    return dict(source=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
        malformed_csv_rows=parsed['malformed_rows'],
        runs=[analyze_run(run, parsed['statuses'], window, len(parsed['runs']) == 1) for run in runs])


def markdown(report):
    lines = ['# 播放器板端日志分析', '', f'比较窗口：快照 `elapsed_ms` 在 0..{report["window_s"]:g} 秒。CPU 按各核真实有效 `window_us` 加权；核窗口边界不同步。', '',
             '| 来源 / 标签 | CSV末采样 | MIPS忙率 / 有效窗秒 | RV忙率 / 有效窗秒 | MIPS / RV最低观测空闲堆 | 完整窗口 | 板内30分钟 | 视频观察 |',
             '| --- | ---: | ---: | ---: | ---: | --- | --- | --- |']
    for file in report['files']:
        for run in file['runs']:
            def cpu(core):
                value = run['cpu'][core]
                busy = '无有效窗' if value['weighted_busy_bp'] is None else f'{value["weighted_busy_bp"] / 100:.3f}%'
                return f'{busy} / {value["weight_us"] / 1000000:.3f}s'
            heap = ' / '.join(str(run['cpu'][core]['observed_heap_free_min_bytes']) if run['cpu'][core]['observed_heap_free_min_bytes'] is not None else '未知' for core in ('mips', 'rv'))
            last = run['sampling']['last_ms']
            elapsed = f'{last / 1000:.3f}s' if last is not None else '仅摘要'
            label = run['label'].replace('|', '\\|')
            video_label = {'needs-review': 'drop/late待复核', 'no-drop-late-observed': '未观察到drop/late', 'not-evaluated': '证据不足'}[run['completion']['video_acceptance']]
            lines.append(f'| {Path(file["source"]).name} / {label} | {elapsed} | {cpu("mips")} | {cpu("rv")} | {heap} | {"完整" if run["sampling"]["csv_window_complete"] else "未证明"} | {"已有证据" if run["completion"]["internal_30min"] == "evidenced" else "未证明"} | {video_label} |')
    for file in report['files']:
        for run in file['runs']:
            lines += ['', f'## {run["label"]}', '', f'原始日志：`{file["source"]}`；SHA256 `{file["sha256"]}`。', '']
            summary = run['source_summaries'][-1] if run['source_summaries'] else None
            if summary:
                lines.append(f'固件摘要：reason={summary.get("reason")}，elapsed_ms={summary.get("elapsed_ms")}，samples={summary.get("samples")}，decoded/displayed={summary.get("decoded")}/{summary.get("displayed")}，drop/late={summary.get("dropped")}/{summary.get("late")}。这是摘要原值，未混入窗口重算。')
                lines += ['', '固件整段摘要CPU/堆：`' + json.dumps({key: summary.get(key) for key in
                    ('mips_windows', 'mips_busy_bp', 'rv_windows', 'rv_busy_bp', 'mips_min_heap', 'rv_min_heap')},
                    ensure_ascii=False) + '`；这组值不等于指定窗口的独立CSV重算。']
            if run['source_session_statuses']:
                status = run['source_session_statuses'][-1]
                lines += ['', f'最后相关会话状态：session={status.get("session")}，state={status.get("state")}，samples={status.get("samples")}，decoded/displayed={status.get("decoded")}/{status.get("displayed")}，path=`{status.get("path", "")}`。']
            lines += ['', '| 计数 | 已观察同scope差分 | 首值 / 末值 | scope切换 / 回绕 / 复位疑点 |', '| --- | ---: | ---: | ---: |']
            for name in COUNTER_SCOPES:
                item = run['counters'][name]
                values = [str(item[key]) if item[key] is not None else '未知' for key in ('observed_delta', 'first_value', 'last_value')]
                lines.append(f'| {name} | {values[0]} | {values[1]} / {values[2]} | {item["scope_changes"]} / {item["wraps"]} / {item["reset_candidates"]} |')
            lines += ['', 'CSV未导出begin基线，首值不当作窗口增量；上述差分不覆盖第一行前的变化。', '',
                      '每秒水位观测：`' + json.dumps(run['watermarks'], ensure_ascii=False) + '`。', '',
                      '错误观测（max / 观测次数）：`' + json.dumps(run['error_observations'], ensure_ascii=False) + '`。', '',
                      '标志计数：`' + json.dumps(run['flags'], ensure_ascii=False) + '`。', '']
            lines.extend('- ' + warning for warning in run['warnings'])
            lines.extend('- ' + issue for issue in run['completion']['issues'])
            video = run['completion']['video']
            lines += ['', '视频累计观测峰值：`' + json.dumps(video['observed_cumulative_peaks'], ensure_ascii=False) + '`。内部完成按 decoded=displayed+dropped 核对；偶发 drop/late 不直接否定完整播放。']
            if video['assessment'] == 'needs-review':
                lines += ['', '视频质量需人工复核。以下为有变化或无法确定的真实采样区间，不设自动“持续性”阈值；完整逐区间列表保存在JSON。', '',
                          '| 区间ms | session | drop差分 | late差分 |', '| --- | ---: | ---: | ---: |']
                for item in video['sample_interval_deltas']:
                    if item['dropped_delta'] != 0 or item['late_delta'] != 0:
                        lines.append(f'| {item["from_ms"]}..{item["to_ms"]} | {item["session"]} | {item["dropped_delta"]} | {item["late_delta"]} |')
    lines += ['', '本报告只评价板内计数和采样证据。手机实拍声光误差 ≤100ms 需独立校准与测量，当前始终为未评价。',
              'CPU口径为scheduler idle；ISR归被打断任务，idle期间的ISR不单独计为busy。历史heap_min可能早于本次测量；表中使用本窗口有效heap_free最小值。', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, nargs='?')
    parser.add_argument('--compare', type=Path, nargs=2, metavar=('MIPS_LOG', 'AMP_LOG'))
    parser.add_argument('--label', help='Only analyze this stored measurement label (single-log mode).')
    parser.add_argument('--window', type=float, default=600)
    parser.add_argument('--output', type=Path, help='JSON output; Markdown uses the same basename.')
    args = parser.parse_args()
    if bool(args.log) == bool(args.compare):
        parser.error('Provide one log, or --compare MIPS_LOG AMP_LOG')
    if not math.isfinite(args.window) or args.window <= 0:
        parser.error('--window must be finite and positive')
    if args.compare and args.label:
        parser.error('--label is for single-log mode; compare retains each log label')
    try:
        files = [analyze_file(path, args.window, args.label) for path in (args.compare or [args.log])]
    except (ValueError, OSError) as error:
        parser.exit(2, f'Analysis failed: {error}\n')
    report = dict(schema_version=1, window_s=args.window, mode='compare' if args.compare else 'single',
                  files=files, physical_sync_100ms='not-evaluated')
    destination = args.output or Path('build/player-board-analysis') / ('comparison.json' if args.compare else args.log.stem + '.json')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    destination.with_suffix('.md').write_text(markdown(report), encoding='utf-8')
    print(json.dumps(dict(json=str(destination.resolve()), markdown=str(destination.with_suffix('.md').resolve()),
        runs=[dict(label=run['label'], csv_window_complete=run['sampling']['csv_window_complete'],
                   internal_30min=run['completion']['internal_30min']) for file in files for run in file['runs']]), ensure_ascii=False))


if __name__ == '__main__':
    main()
