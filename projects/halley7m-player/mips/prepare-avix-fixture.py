"""Repack the real 3s sync AVI across sparse 1/2GiB AVIX boundaries and verify it.

The holes are valid JUNK chunks. Copying the result to FAT32 materializes the
zeros, so this diagnostic occupies slightly over 2GiB on a physical card. No
media/session/storage source is changed and no card or serial port is opened.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[3]
GIB = 1 << 30
FRAMES = 90
SAMPLES = 144000
PCM_FRAME_BYTES = 1600 * 2


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def chunk(tag, payload):
    return tag + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) & 1)


def listing(kind, payload):
    return chunk(b'LIST', kind + payload)


def source_payloads(media, path):
    """Read only packet/header bytes, including a source that already has AVIX."""
    video, audio = [], bytearray()
    hdrl = None
    with path.open('rb') as handle:
        first = media.read_exact(handle, 0, 12)
        for tag, offset, length in media.chunks(handle, 12, 8 + media.u32(first, 4)):
            if tag == b'LIST' and media.read_exact(handle, offset, 4) == b'hdrl':
                hdrl = (offset + 4, offset + length)
                streams = media.stream_headers(handle, *hdrl)
                break
        if hdrl is None:
            raise ValueError('Missing source hdrl')
        video_index = next(index for index, stream in enumerate(streams) if stream['type'] == b'vids')
        audio_index = next(index for index, stream in enumerate(streams) if stream['type'] == b'auds')
        video_tag, audio_tag = f'{video_index:02d}dc'.encode(), f'{audio_index:02d}wb'.encode()
        children = []
        for tag, offset, length in media.chunks(handle, *hdrl):
            if tag == b'avih':
                header = bytearray(media.read_exact(handle, offset, length))
                # avih describes the first RIFF; strh/dmlh retain full duration.
                struct.pack_into('<I', header, 16, 30)
                struct.pack_into('<I', header, 12, media.u32(header, 12) & ~0x10)
                children.append(chunk(tag, header))
            elif tag == b'LIST' and media.read_exact(handle, offset, 4) == b'strl':
                stream = b''
                for child, position, size in media.chunks(handle, offset + 4, offset + length):
                    if child in (b'strh', b'strf', b'vprp'):
                        stream += chunk(child, media.read_exact(handle, position, size))
                children.append(listing(b'strl', stream))
        # AVI extended header: total frames, followed by its reserved fields.
        children.append(listing(b'odml', chunk(b'dmlh', struct.pack('<I', FRAMES) + bytes(244))))
        header = listing(b'hdrl', b''.join(children))

        def movi(start, end):
            for tag, offset, length in media.chunks(handle, start, end):
                if tag == b'LIST' and media.read_exact(handle, offset, 4) == b'rec ':
                    movi(offset + 4, offset + length)
                elif tag == video_tag:
                    video.append(media.read_exact(handle, offset, length))
                elif tag == audio_tag:
                    audio.extend(media.read_exact(handle, offset, length))

        position = 0
        while position < path.stat().st_size:
            root = media.read_exact(handle, position, 12)
            end = position + 8 + media.u32(root, 4)
            for tag, offset, length in media.chunks(handle, position + 12, end):
                if tag == b'LIST' and media.read_exact(handle, offset, 4) == b'movi':
                    movi(offset + 4, offset + length)
            position = end + (end & 1)
    if len(video) != FRAMES or len(audio) != SAMPLES * 2:
        raise ValueError('Source must be the exact 90-frame/144000-sample diagnostic')
    return header, video_tag, audio_tag, video, bytes(audio)


def build_parts(header, video_tag, audio_tag, video, pcm):
    parts, layout = [], []
    position = 0
    for segment in range(3):
        prefix = header if segment == 0 else b''
        records = b''.join(listing(b'rec ', chunk(video_tag, video[index]) +
            chunk(audio_tag, pcm[index * PCM_FRAME_BYTES:(index + 1) * PCM_FRAME_BYTES]))
            for index in range(segment * 30, (segment + 1) * 30))
        # End the first two RIFFs exactly at the 1/2GiB boundaries. JUNK is
        # inside movi, so both indexed and sequential demuxers can skip it.
        total = GIB if segment < 2 else 12 + len(prefix) + 12 + len(records)
        junk = total - 12 - len(prefix) - 12 - len(records) - 8 if segment < 2 else 0
        movi_length = total - 12 - len(prefix) - 8
        parts.extend([b'RIFF' + struct.pack('<I', total - 8) + (b'AVI ' if not segment else b'AVIX'),
                      prefix, b'LIST' + struct.pack('<I', movi_length) + b'movi', records])
        junk_offset = position + 12 + len(prefix) + 12 + len(records) if junk else None
        if junk:
            if junk < 0 or junk & 1:
                raise ValueError('Invalid sparse JUNK size')
            parts.extend([b'JUNK' + struct.pack('<I', junk), junk])
        layout.append(dict(riff=segment, type='AVI ' if not segment else 'AVIX',
            offset=position, end=position + total, bytes=total, video_first=segment * 30,
            video_frames=30, audio_samples=48000,
            first_video_chunk_header=position + 12 + len(prefix) + 24,
            junk_header=junk_offset, junk_payload_bytes=junk))
        position += total
    if not 2 * GIB < position < 1 << 32:
        raise ValueError('Expected a >2GiB and <4GiB diagnostic')
    return parts, layout


def frame_crc(ffmpeg, path):
    output = subprocess.check_output([str(ffmpeg), '-v', 'error', '-err_detect', 'explode',
        '-i', str(path), '-map', '0:v:0', '-an', '-pix_fmt', 'nv12', '-f', 'framecrc', '-'], text=True)
    return [line.strip() for line in output.splitlines() if line and not line.startswith('#')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg')
    parser.add_argument('--source', type=Path, default=ROOT / 'build/player-fixture/sync-flash-tone.avi')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/player-fixture/avix')
    args = parser.parse_args()
    media = module('avix_media', ROOT / 'scripts/prepare-player-media.py')
    support = module('avix_test', ROOT / 'scripts/test-player-media.py')
    acceptance = module('avix_native', Path(__file__).with_name('prepare-acceptance-fixture.py'))
    ffmpeg = media.find_ffmpeg(args.ffmpeg)
    source, output = args.source.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = media.validate_media(source)
    header, vt, at, video, pcm = source_payloads(media, source)
    parts, layout = build_parts(header, vt, at, video, pcm)
    destination = output / 'sync-flash-tone-over-2gib.avi'
    print('Writing sparse AVI/AVIX; media payloads are unchanged', flush=True)
    support.sparse_file(destination, parts)
    contract = media.validate_media(destination)
    if (contract['video_frames'], contract['audio_samples'], contract['riff_count']) != (FRAMES, SAMPLES, 3):
        raise ValueError('Complete AVIX parser counts changed')
    repacked = source_payloads(media, destination)
    if repacked[3] != video or repacked[4] != pcm:
        raise ValueError('Repacked video AU/PCM payloads changed')
    print('Building/running native streaming parser across both large boundaries', flush=True)
    native_parser = acceptance.compile_parser(support, output)
    native = json.loads(subprocess.check_output([str(native_parser), str(destination)], text=True))
    if native['frames'] != FRAMES or native['samples'] != SAMPLES:
        raise ValueError('Native parser stopped before the full AVIX duration')
    print('Checking FFprobe packets and complete PC frame CRC/PCM decode', flush=True)
    media.verify_codecs(destination, ffmpeg)
    ffprobe = ffmpeg.with_name('ffprobe' + ffmpeg.suffix)
    probe = json.loads(subprocess.check_output([str(ffprobe), '-v', 'error', '-show_packets',
        '-show_entries', 'packet=stream_index,pos,size', '-of', 'json', str(destination)], text=True))
    packets = probe['packets']
    vp = [packet for packet in packets if packet['stream_index'] == int(vt[:2])]
    ap = [packet for packet in packets if packet['stream_index'] == int(at[:2])]
    if len(vp) != FRAMES or sum(int(packet['size']) for packet in ap) != len(pcm):
        raise ValueError('FFprobe did not read all AU/PCM packets')
    for segment in range(3):
        count = sum(layout[segment]['offset'] <= int(packet['pos']) < layout[segment]['end'] for packet in vp)
        if count != 30:
            raise ValueError(f'FFprobe segment {segment} packet count {count}')
    source_crc, actual_crc = frame_crc(ffmpeg, source), frame_crc(ffmpeg, destination)
    if len(actual_crc) != FRAMES or actual_crc != source_crc:
        raise ValueError('Decoded NV12 frame CRC/timestamps differ from the source')
    decoded_pcm = subprocess.check_output([str(ffmpeg), '-v', 'error', '-err_detect', 'explode',
        '-i', str(destination), '-map', '0:a:0', '-vn', '-c:a', 'pcm_s16le', '-f', 's16le', '-'])
    if decoded_pcm != pcm:
        raise ValueError('Decoded PCM differs from the original exact samples')
    manifest = dict(file=str(destination), bytes=destination.stat().st_size, sparse_on_host=True,
        file_sha256=None, file_sha256_note='Not read through 2GiB of JUNK; payloads and chunk layout are verified.',
        source=dict(path=str(source), bytes=source.stat().st_size,
                    sha256=hashlib.sha256(source.read_bytes()).hexdigest(), contract=original),
        layout=layout, first_riff_avih_frames=30, complete_strh_and_dmlh_frames=FRAMES,
        contract=contract, native=native, ffprobe_video_packets=len(vp), ffprobe_audio_bytes=len(pcm),
        payload=dict(video_au_sha256=[hashlib.sha256(au).hexdigest() for au in video],
                     video_au_bytes=[len(au) for au in video], pcm_sha256=hashlib.sha256(pcm).hexdigest(),
                     pcm_bytes=len(pcm), all_source_payloads_exact=True),
        decoded_nv12_framecrc=actual_crc, decoded_pcm_exact=True,
        note='PC-only validation. FAT32 copy uses >2GiB of actual capacity; no physical card/hardware test performed.')
    (output / 'avix-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(dict(file=str(destination), bytes=manifest['bytes'], contract=contract, native=native,
                         decoded_frames=len(actual_crc), decoded_pcm_exact=True), ensure_ascii=False))


if __name__ == '__main__':
    main()
