"""Compile the bounded MP4 demux and verify real FFmpeg MP4/M4A packets.

All generated media and decoded outputs stay under build/player-host/mp4.
This tool never accesses a serial port or modifies the SDK.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from fractions import Fraction

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, **kwargs)
    if result.returncode:
        for value in (result.stdout, result.stderr):
            if value:
                print(value if isinstance(value, str) else value.decode(errors='replace'))
        result.check_returncode()
    return result


def compile_host(output, cc):
    source = ROOT / 'projects/halley7m-player/common'
    executable = output / ('player_mp4_test.exe' if os.name == 'nt' else 'player_mp4_test')
    test = ROOT / 'tests/player_mp4_test.c'
    if Path(cc).stem.lower() == 'cl':
        spec = importlib.util.spec_from_file_location('amp_test', ROOT / 'scripts/test-player-amp.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        environment = module.msvc_environment()
        compiler = shutil.which(cc, path=environment.get('Path', environment.get('PATH'))) or cc
        result = run([compiler, '/nologo', '/std:c11', '/O2', '/W4', '/WX', '/utf-8',
                      '/I' + str(source), test, source / 'player_mp4.c', '/Fe:' + str(executable)],
                     cwd=output, env=environment)
    else:
        result = run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', source,
                      test, source / 'player_mp4.c', '-o', executable], cwd=output)
    if result.stdout:
        print(result.stdout.decode(errors='replace').strip())
    return executable


CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts'}


def boxes(data):
    pos = 0
    while pos < len(data):
        size, tag = struct.unpack_from('>I4s', data, pos)
        header = 8
        if size == 1:
            size, = struct.unpack_from('>Q', data, pos + 8)
            header = 16
        elif not size:
            size = len(data) - pos
        assert size >= header and pos + size <= len(data)
        yield tag, data[pos + header:pos + size]
        pos += size


def box(tag, payload):
    return struct.pack('>I4s', len(payload) + 8, tag) + payload


def rewrite(data, change):
    result = bytearray()
    for tag, payload in boxes(data):
        if tag in CONTAINERS:
            payload = rewrite(payload, change)
        tag, payload = change(tag, payload)
        result += box(tag, payload)
    return bytes(result)


def quantized_frame_times(data):
    """Replace a CFR30 video STTS with repeating 33/33/34 ms intervals.

    The 90 kHz track and full-duration edit stay unchanged. Only the timing
    table changes; coded samples and their offsets are preserved because
    these fixtures keep moov after mdat.
    """
    video = False

    def change(tag, payload):
        nonlocal video
        if tag == b'hdlr':
            video = payload[8:12] == b'vide'
        elif tag == b'stts' and video:
            entries, = struct.unpack_from('>I', payload, 4)
            rows = [struct.unpack_from('>II', payload, 8 + 8 * i) for i in range(entries)]
            assert all(delta == 3000 for _, delta in rows)
            count = sum(n for n, _ in rows)
            assert count % 3 == 0
            durations = [2970, 2970, 3060] * (count // 3)
            runs = []
            for duration in durations:
                if runs and runs[-1][1] == duration:
                    runs[-1] = (runs[-1][0] + 1, duration)
                else:
                    runs.append((1, duration))
            payload = payload[:4] + struct.pack('>I', len(runs))
            payload += b''.join(struct.pack('>II', *row) for row in runs)
        return tag, payload

    return rewrite(data, change)


def sparse_file(path, prefix, payload):
    with path.open('wb') as handle:
        if os.name == 'nt':
            import ctypes
            import msvcrt
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
                                               ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                               ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
            returned = ctypes.c_ulong()
            if not kernel.DeviceIoControl(msvcrt.get_osfhandle(handle.fileno()), 0x900C4,
                                          None, 0, None, 0, ctypes.byref(returned), None):
                raise OSError(ctypes.get_last_error(), 'cannot mark MP4 test file sparse')
        handle.write(struct.pack('>I4sQ', 1, b'free', prefix))
        handle.seek(prefix)
        handle.write(payload)


def probe(ffprobe, path):
    return json.loads(run([ffprobe, '-v', 'error', '-show_streams', '-show_packets',
                           '-of', 'json', path], text=True).stdout)


def framemd5(ffmpeg, path):
    data = run([ffmpeg, '-v', 'error', '-i', path, '-map', '0:v:0', '-fps_mode', 'passthrough',
                '-f', 'framemd5', '-'], text=True).stdout
    return [line.rsplit(',', 1)[-1].strip() for line in data.splitlines() if not line.startswith('#')]


def check_media(executable, ffmpeg, ffprobe, path, output, decode=True):
    prefix = output / path.stem
    records = [json.loads(line) for line in run([executable, path, prefix], text=True).stdout.splitlines()]
    info = records[0]['info']
    packets = [row['packet'] for row in records if 'packet' in row]
    expected = probe(ffprobe, path)
    streams = {int(stream['index']): stream for stream in expected['streams']}
    track_counts = {1: 0, 2: 0}
    dts_bias = {}
    expected_by_kind = {1: [], 2: []}
    source_audio = bytearray()
    with path.open('rb') as handle:
        for packet in expected['packets']:
            stream = streams[int(packet['stream_index'])]
            kind = 1 if stream['codec_type'] == 'video' else 2
            expected_by_kind[kind].append((stream, packet))
    for packet in packets:
        kind = packet['kind']
        stream, reference = expected_by_kind[kind][track_counts[kind]]
        track_counts[kind] += 1
        assert packet['offset'] == int(reference['pos'])
        assert packet['source'] == int(reference['size'])
        assert packet['index'] == track_counts[kind] - 1
        scale = Fraction(stream['time_base'])
        # ffprobe exposes the edited stream timeline. The demux also retains
        # the unedited ticks separately for decoder priming/trim accounting.
        assert abs(Fraction(packet['pts_us'], 1000000) - int(reference['pts']) * scale) < Fraction(2, 1000000), (path, packet, reference)
        # FFmpeg may rebase DTS below zero for signed CTTS, while preserving
        # PTS. Our raw DTS remains the stts sum. Require an invariant bias,
        # rather than mistaking that decoder scheduling choice for PTS drift.
        bias = Fraction(packet['dts'], packet['scale']) - int(reference['dts']) * scale
        dts_bias.setdefault(kind, bias)
        assert bias == dts_bias[kind], (path, packet, reference)
        if not info['video' if kind == 1 else 'audio']['ctts']:
            assert abs(Fraction(packet['dts_us'], 1000000) - int(reference['dts']) * scale) < Fraction(2, 1000000)
        assert abs(Fraction(packet['duration_us'], 1000000) - int(reference['duration']) * scale) < Fraction(2, 1000000)
        if kind == 2:
            with path.open('rb') as handle:
                handle.seek(packet['offset'])
                source_audio += handle.read(packet['source'])
    assert all(a['dts_us'] <= b['dts_us'] for a, b in zip(packets, packets[1:]))
    assert records[-1]['done']['video'] == len(expected_by_kind[1])
    assert records[-1]['done']['audio'] == len(expected_by_kind[2])
    if info['has_video']:
        video = info['video']
        visible = [packet for packet in packets if packet['kind'] == 1 and
                   packet['pts_us'] + packet['duration_us'] > video['presentation_start_us'] and
                   packet['pts_us'] < video['presentation_end_us']]
        assert len(visible) == video['presentation_samples']
        if visible:
            assert max(packet['pts_us'] for packet in visible) == video['presentation_last_pts_us']
    assert prefix.with_suffix('.aacraw').read_bytes() == source_audio
    if info['has_video'] and decode:
        assert framemd5(ffmpeg, path) == framemd5(ffmpeg, prefix.with_suffix('.h264'))
    print(f"PASS {path.name}: video={track_counts[1]} audio={track_counts[2]}")
    return info, packets, expected


def expect_error(executable, path, output, code=None):
    result = subprocess.run([str(executable), str(path), str(output / path.stem)], capture_output=True, text=True)
    assert result.returncode == 2, (path, result.stdout, result.stderr)
    record = json.loads(result.stdout.splitlines()[-1])
    assert 'error' in record
    if code is not None:
        assert record['error'] == code, record
    print(f"PASS reject {path.name}: {record}")


def noninterleaved(path, original, reference, multiple=False):
    top = dict(boxes(original))
    streams = reference['streams']
    offsets = {}
    payload = bytearray()
    media = bytearray()
    start = len(box(b'ftyp', top[b'ftyp'])) + 8
    for stream in streams:
        index = int(stream['index'])
        offsets[index] = []
        for packet in reference['packets']:
            if int(packet['stream_index']) == index:
                offsets[index].append(start + len(payload))
                pos, size = int(packet['pos']), int(packet['size'])
                payload += original[pos:pos + size]
        if multiple:
            media += box(b'mdat', payload)
            payload.clear()
            start = len(box(b'ftyp', top[b'ftyp'])) + len(media) + 8
    track_index = -1

    def change(tag, data):
        nonlocal track_index
        if tag == b'tkhd':
            track_index += 1
        if tag in (b'stco', b'co64'):
            data = b'\0' * 4 + struct.pack('>I', len(offsets[track_index]))
            data += b''.join(struct.pack('>I', pos) for pos in offsets[track_index])
            tag = b'stco'
        elif tag == b'stsc':
            data = struct.pack('>IIIII', 0, 1, 1, 1, 1)
        return tag, data
    moov = rewrite(box(b'moov', top[b'moov']), change)
    if not multiple:
        media = box(b'mdat', payload)
    path.write_bytes(box(b'ftyp', top[b'ftyp']) + media + moov)


def prefix_fixture(path, original, reference, width):
    top = dict(boxes(original))
    payload = bytearray()
    sizes, offsets = [], []
    start = len(box(b'ftyp', top[b'ftyp'])) + 8
    for packet in reference['packets']:
        assert packet['codec_type'] == 'video'
        pos, size = int(packet['pos']), int(packet['size'])
        data = original[pos:pos + size]
        offsets.append(start + len(payload))
        before = len(payload)
        pos = 0
        while pos < len(data):
            n, = struct.unpack_from('>I', data, pos)
            assert n < (1 << (8 * width))
            payload += n.to_bytes(width, 'big') + data[pos + 4:pos + 4 + n]
            pos += 4 + n
        sizes.append(len(payload) - before)

    def change(tag, data):
        if tag == b'avcC':
            data = data[:4] + bytes([(data[4] & 0xfc) | (width - 1)]) + data[5:]
        elif tag == b'stsz':
            data = struct.pack('>III', 0, 0, len(sizes)) + b''.join(struct.pack('>I', n) for n in sizes)
        elif tag == b'stsc':
            data = struct.pack('>IIIII', 0, 1, 1, 1, 1)
        elif tag == b'stco':
            data = struct.pack('>II', 0, len(offsets)) + b''.join(struct.pack('>I', n) for n in offsets)
        return tag, data
    # avcC is nested inside a visual sample entry, not a generic container.
    def stsd_change(tag, data):
        if tag == b'stsd':
            sample_type, sample_data = next(boxes(data[8:]))
            data = data[:8] + box(sample_type, sample_data[:78] + rewrite(sample_data[78:], change))
        return change(tag, data)
    path.write_bytes(box(b'ftyp', top[b'ftyp']) + box(b'mdat', payload) + rewrite(box(b'moov', top[b'moov']), stsd_change))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cl' if os.name == 'nt' else 'gcc'))
    parser.add_argument('--ffmpeg', type=Path, default=ROOT / 'tools/ffmpeg/bin/ffmpeg.exe')
    args = parser.parse_args()
    output = ROOT / 'build/player-host/mp4'
    output.mkdir(parents=True, exist_ok=True)
    executable = compile_host(output, args.cc)
    ffmpeg = args.ffmpeg.resolve()
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    ff = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y']
    sample = output / 'ordinary.mp4'
    run(ff + [
         '-f', 'lavfi', '-i', 'testsrc2=s=1280x720:r=30:d=1',
         '-f', 'lavfi', '-i', 'sine=f=997:r=48000:d=1',
         '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
         '-x264-params', 'bframes=0:ref=1:keyint=30:scenecut=0', '-pix_fmt', 'yuv420p',
         '-video_track_timescale', '90000', '-c:a', 'aac', '-b:a', '96k', sample])
    info, packets, reference = check_media(executable, ffmpeg, ffprobe, sample, output)
    assert info['video']['width'] == 1280 and info['video']['height'] == 720
    assert info['audio']['media_start'] == 1024
    assert info['audio']['edit_duration'] == 48000
    original = sample.read_bytes()
    fast = output / 'faststart.mp4'
    run(ff + ['-i', sample, '-c', 'copy', '-movflags', '+faststart', fast])
    check_media(executable, ffmpeg, ffprobe, fast, output)

    for rate in ('24', '25', '30000/1001'):
        path = output / ('timestamps-' + rate.replace('/', '-') + '.mp4')
        run(ff + ['-f', 'lavfi', '-i', f'color=c=blue:s=1280x720:r={rate}:d=1.2',
                  '-f', 'lavfi', '-i', 'sine=f=997:r=48000:d=1.2',
                  '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.1',
                  '-refs', '1', '-bf', '0', '-pix_fmt', 'yuv420p', '-c:a', 'aac', path])
        _, timed, _ = check_media(executable, ffmpeg, ffprobe, path, output)
        video = [packet for packet in timed if packet['kind'] == 1]
        assert any(packet['pts_us'] != packet['index'] * 1000000 // 30 for packet in video)

    quantized = output / 'timestamps-33-34ms.mp4'
    quantized.write_bytes(quantized_frame_times(original))
    _, timed, _ = check_media(executable, ffmpeg, ffprobe, quantized, output)
    assert {p['duration_us'] for p in timed if p['kind'] == 1} == {33000, 34000}

    audio = output / 'stereo-44100.m4a'
    run(ff + ['-f', 'lavfi', '-i', 'sine=f=811:r=44100:d=1.013', '-ac', '2', '-c:a', 'aac', audio])
    ai, _, _ = check_media(executable, ffmpeg, ffprobe, audio, output)
    assert ai['has_video'] == 0 and ai['audio']['rate'] == 44100 and ai['audio']['channels'] == 2

    vfr = output / 'vfr.mp4'
    run(ff + ['-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30:d=8',
              '-vf', r'select=not(eq(mod(n\,3)\,1))', '-fps_mode', 'vfr',
              '-c:v', 'libx264', '-profile:v', 'baseline', '-bf', '0', vfr])
    vi, vp, _ = check_media(executable, ffmpeg, ffprobe, vfr, output)
    assert vi['has_audio'] == 0 and len({p['duration'] for p in vp}) > 1
    assert len(vp) > 128  # Crosses both stsz and stts 512-byte page boundaries.

    bframes = output / 'ctts-bframes.mp4'
    run(ff + ['-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30:d=1', '-c:v', 'libx264',
              '-profile:v', 'main', '-bf', '2', bframes])
    bi, bp, _ = check_media(executable, ffmpeg, ffprobe, bframes, output)
    assert bi['video']['ctts'] == 1 and bi['video']['profile'] == 77 and any(p['pts'] != p['dts'] for p in bp)

    signed = output / 'ctts-signed.mp4'
    run(ff + ['-i', bframes, '-c', 'copy', '-movflags', '+negative_cts_offsets', signed])
    check_media(executable, ffmpeg, ffprobe, signed, output)

    delayed = output / 'empty-edit.mp4'
    run(ff + ['-itsoffset', '0.2', '-i', sample, '-c', 'copy', delayed])
    di, _, _ = check_media(executable, ffmpeg, ffprobe, delayed, output)
    assert di['video']['empty_duration'] > 0

    # Convert real stco tables to co64, preserving the real coded payloads.
    def co64(tag, payload, offset=0):
        if tag == b'stco':
            count, = struct.unpack_from('>I', payload, 4)
            entries = struct.unpack_from('>' + 'I' * count, payload, 8)
            return b'co64', payload[:8] + b''.join(struct.pack('>Q', value + offset) for value in entries)
        return tag, payload
    co = output / 'co64.mp4'
    co.write_bytes(rewrite(original, co64))
    check_media(executable, ffmpeg, ffprobe, co, output)

    # This is a sparse host IO64 test, beyond production FAT32's file limit.
    large = output / 'co64-over4gib.mp4'
    prefix = (1 << 32) + 16
    sparse_file(large, prefix, rewrite(original, lambda tag, data: co64(tag, data, prefix)))
    check_media(executable, ffmpeg, ffprobe, large, output)

    non = output / 'noninterleaved.mp4'
    noninterleaved(non, original, reference)
    _, np, _ = check_media(executable, ffmpeg, ffprobe, non, output)
    assert [p['kind'] for p in np] == [p['kind'] for p in packets]
    multi = output / 'multiple-mdat.mp4'
    noninterleaved(multi, original, reference, multiple=True)
    check_media(executable, ffmpeg, ffprobe, multi, output)

    paged = output / 'paged-av.mp4'
    run(ff + ['-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30:d=8',
              '-f', 'lavfi', '-i', 'sine=f=503:r=48000:d=8', '-c:v', 'libx264',
              '-profile:v', 'baseline', '-bf', '0', '-c:a', 'aac', paged])
    pi, _, _ = check_media(executable, ffmpeg, ffprobe, paged, output)
    assert pi['video']['samples'] > 128 and pi['audio']['samples'] > 256

    tiny = output / 'tiny.mp4'
    run(ff + ['-f', 'lavfi', '-i', 'testsrc2=s=16x16:r=30:d=1', '-c:v', 'libx264',
              '-profile:v', 'baseline', '-bf', '0', '-bsf:v', 'filter_units=remove_types=6', tiny])
    tr = probe(ffprobe, tiny)
    for width in (1, 2):
        prefixed = output / f'avcc-prefix{width}.mp4'
        prefix_fixture(prefixed, tiny.read_bytes(), tr, width)
        ni, _, _ = check_media(executable, ffmpeg, ffprobe, prefixed, output)
        assert ni['video']['nal_length'] == width

    repeated = output / 'aud-repeat-headers.mp4'
    run(ff + ['-f', 'lavfi', '-i', 'testsrc2=s=320x240:r=30:d=1.1', '-c:v', 'libx264',
              '-profile:v', 'baseline', '-x264-params', 'bframes=0:aud=1:repeat-headers=1:keyint=15', repeated])
    check_media(executable, ffmpeg, ffprobe, repeated, output)

    version1 = output / 'version1-times.mp4'
    def version1_times(tag, data):
        if tag in (b'mvhd', b'mdhd'):
            creation, modification, scale, duration = struct.unpack_from('>IIII', data, 4)
            data = b'\1' + data[1:4] + struct.pack('>QQIQ', creation, modification, scale, duration) + data[20:]
        elif tag == b'elst':
            count, = struct.unpack_from('>I', data, 4)
            entries = [struct.unpack_from('>IiI', data, 8 + 12 * i) for i in range(count)]
            data = b'\1' + data[1:8] + b''.join(struct.pack('>QqI', *entry) for entry in entries)
        return tag, data
    version1.write_bytes(rewrite(original, version1_times))
    check_media(executable, ffmpeg, ffprobe, version1, output)

    fixed = output / 'fixed-stsz.mp4'
    run(ff + ['-i', tiny, '-frames:v', '1', '-c', 'copy', fixed])
    def fixed_size(tag, data):
        if tag == b'stsz':
            size, count = struct.unpack_from('>II', data, 4)
            assert count == 1
            if not size:
                data = data[:4] + data[12:16] + data[8:12]
        return tag, data
    fixed.write_bytes(rewrite(fixed.read_bytes(), fixed_size))
    check_media(executable, ffmpeg, ffprobe, fixed, output)

    # Same production callbacks deliberately return only seven bytes per read.
    run([executable, sample, output / 'short-read', 'short-read'], text=True)
    assert (output / 'short-read.h264').read_bytes() == (output / 'ordinary.h264').read_bytes()
    assert (output / 'short-read.aacraw').read_bytes() == (output / 'ordinary.aacraw').read_bytes()
    print('PASS IO short reads and identical normalized output')

    fragment = output / 'fragmented.mp4'
    run(ff + ['-i', sample, '-c', 'copy', '-movflags', '+frag_keyframe+empty_moov', fragment])
    expect_error(executable, fragment, output, -3)

    bad_codec = output / 'hevc-not-avc1.mp4'
    bad_codec.write_bytes(original.replace(b'avc1', b'hev1'))
    expect_error(executable, bad_codec, output, -3)

    bad_edit = output / 'edit-rate2.mp4'
    def rate2(tag, data):
        if tag == b'elst':
            data = data[:-4] + struct.pack('>I', 0x20000)
        return tag, data
    bad_edit.write_bytes(rewrite(original, rate2))
    expect_error(executable, bad_edit, output, -3)

    complex_edit = output / 'edit-montage.mp4'
    def montage(tag, data):
        if tag == b'elst':
            data = data[:4] + struct.pack('>I', 2) + data[8:] * 2
        return tag, data
    complex_edit.write_bytes(rewrite(original, montage))
    expect_error(executable, complex_edit, output, -3)

    # LC outer object type + explicit SBR sync extension must not be silently
    # advertised as LC. The coded payload stays real AAC; config is a negative.
    sbr = output / 'aac-sbr-extension.mp4'
    asc = (output / 'ordinary.asc').read_bytes()
    assert len(asc) == 5
    changed = asc[:4] + bytes([asc[4] | 0x80])
    assert original.count(asc) == 1
    sbr.write_bytes(original.replace(asc, changed))
    expect_error(executable, sbr, output, -3)

    bad_nal = output / 'invalid-nal.mp4'
    changed = bytearray(original)
    first = next(p for p in packets if p['kind'] == 1)
    struct.pack_into('>I', changed, first['offset'], first['source'] + 1)
    bad_nal.write_bytes(changed)
    expect_error(executable, bad_nal, output, -2)

    truncated = output / 'truncated.mp4'
    truncated.write_bytes(original[:-1])
    expect_error(executable, truncated, output, -2)
    print('MP4: 21 real decode/packet cases, short-read IO, and 7 rejection cases passed')


if __name__ == '__main__':
    main()
