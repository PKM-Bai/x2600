"""Compare uncached FELIX NV12 checksums with independently decoded PC frames.

The UART log must contain one completed `video verify` run. This validates
decoded pixels, not display colors, presentation timing or audio output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re


def check(log, reference):
    document = json.loads(reference.read_text(encoding='utf-8'))
    expected = document['frames'] if isinstance(document, dict) else document
    if not isinstance(expected, list) or not expected:
        raise ValueError('Reference must contain a nonempty frame list')
    text = log.read_text(encoding='utf-8', errors='replace')
    rows = [dict((k, v) for k, v in re.findall(r'(\w+)=(-?[\da-f]+)', line))
            for line in text.splitlines() if line.startswith('VIDEO_FRAME_CRC ')]
    summaries = [line for line in text.splitlines() if line.startswith('VIDEO_PROBE result=')]
    if len(summaries) != 1:
        raise ValueError('Expected exactly one completed video verify summary')
    summary = dict(re.findall(r'(\w+)=(-?\d+)', summaries[0]))
    if int(summary['result']) or int(summary['stop']):
        raise ValueError('Decoder/STOP failed: ' + summaries[0])
    if len(rows) != len(expected) or int(summary['verified']) != len(expected):
        raise ValueError(f'Incomplete frames: board={len(rows)}, reference={len(expected)}')
    previous_pts = None
    for order, (actual, wanted) in enumerate(zip(rows, expected)):
        index = int(actual.get('output_index', actual['index']))
        wanted_index = wanted.get('output_index', wanted.get('index', wanted.get('frame', -1)))
        if index != order or wanted_index != order:
            raise ValueError(f'Output order/duplicate mismatch at frame {order}')
        if 'output_index' in wanted and int(actual['index']) != wanted['index']:
            raise ValueError(f'Decode identity lost during reorder at output {order}')
        for plane in ('y_crc32', 'uv_crc32'):
            if int(actual[plane], 16) != int(wanted[plane], 16):
                raise ValueError(f'Pixel mismatch frame {order} {plane}: {actual[plane]} != {wanted[plane]}')
        pts = int(actual['pts_us'])
        for timestamp in ('pts_us', 'duration_us'):
            if timestamp in wanted and abs(int(actual[timestamp]) - wanted[timestamp]) > 1:
                raise ValueError(f'Timestamp mismatch frame {order}: {timestamp}')
        if previous_pts is not None and pts <= previous_pts:
            raise ValueError(f'Non-increasing presentation PTS at frame {order}')
        previous_pts = pts
    if any(marker in text for marker in ('AMP_FAIL ', 'PANIC:', 'assertion failed', 'VIDEO_PINNED ')):
        raise ValueError('Hardware fault/pinned resource in log')
    return dict(frames=len(rows), pixel_planes=len(rows) * 2,
                log_sha256=hashlib.sha256(log.read_bytes()).hexdigest(),
                reference_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                summary=summary, scope='Decoded NV12 pixels and ordered PTS; no display/audio claim')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = check(args.log, args.reference)
    output = json.dumps(report, indent=2) + '\n'
    if args.report:
        with args.report.open('x', encoding='utf-8') as target:
            target.write(output)
    print(output, end='')


if __name__ == '__main__':
    main()
