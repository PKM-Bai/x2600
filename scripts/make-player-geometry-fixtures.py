"""Isolated step4 board fixtures: coded padding, SAR and four SDR CSC cases.

This generator writes only build artifacts. It does not alter production
sources, access a serial port, or control audio hardware.
"""
from __future__ import annotations
import argparse
from array import array
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT=Path(__file__).resolve().parents[1]
SECONDS,FPS=3,30


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command):
    result=subprocess.run(list(map(str,command)),capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{command}\n'+result.stderr.decode(errors='replace')[-12000:])
    return result.stdout


def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''): h.update(chunk)
    return h.hexdigest()


def save(path,data):
    path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def source_video(ffmpeg,output):
    shared=ROOT/'build/player-fixture/formats/_sources/pattern-lossless.mkv'
    if shared.is_file():
        return shared,{'file':str(shared),'reused':True,'sha256':sha(shared),
                       'used_frames':90,'note':'Only first3s decoded; no duplicate raw RGB source.'}
    pattern=load('geometry_pattern',ROOT/'scripts/make-player-probe-media.py')
    source=output/'_sources/pattern-3s.mkv'
    command=[ffmpeg,'-v','error','-y','-f','rawvideo','-pix_fmt','rgb24',
             '-video_size','1280x720','-framerate','30','-i','pipe:0',
             '-an','-c:v','ffv1','-level','3','-pix_fmt','bgr0','-threads:v','1',
             '-map_metadata','-1','-fflags','+bitexact','-flags:v','+bitexact',source]
    raw_sha=hashlib.sha256()
    with (output/'_reports/source-encode.log').open('wb') as log:
        p=subprocess.Popen(list(map(str,command)),stdin=subprocess.PIPE,stderr=log,stdout=subprocess.DEVNULL)
        try:
            for i in range(90):
                pixels=pattern.picture(i); raw_sha.update(pixels); p.stdin.write(pixels)
        finally: p.stdin.close()
        if p.wait(): raise RuntimeError('Source encode failed; see _reports/source-encode.log')
    return source,{'file':str(source),'reused':False,'sha256':sha(source),
                   'used_frames':90,'rgb24_stream_sha256':raw_sha.hexdigest()}


def trace_sps(ffmpeg,path,report_path,width,height,matrix,full,sar):
    result=subprocess.run(list(map(str,[ffmpeg,'-v','info','-i',path,'-map','0:v:0','-frames:v','1',
                                       '-c:v','copy','-bsf:v','trace_headers','-f','null','-'])),
                          capture_output=True,check=True)
    text=result.stderr.decode(errors='replace')
    report_path.write_text(text,encoding='utf-8')
    def values(field):
        return [int(v) for v in re.findall(r'\b'+field+r'\s+[^\r\n]*=\s*(\d+)',text)]
    def get(field,default=None):
        data=values(field)
        if not data:
            assert default is not None,field
            return default
        assert len(set(data))==1,(field,data)
        return data[0]
    for field,expected in [('profile_idc',66),('level_idc',31),('max_num_ref_frames',1),
                           ('entropy_coding_mode_flag',0),('frame_mbs_only_flag',1),
                           ('video_signal_type_present_flag',1),('video_full_range_flag',int(full)),
                           ('colour_description_present_flag',1),('matrix_coefficients',matrix),
                           ('colour_primaries',matrix),('transfer_characteristics',matrix)]:
        assert get(field)==expected,(path.name,field,get(field),expected)
    cw=(get('pic_width_in_mbs_minus1')+1)*16
    ch=(get('pic_height_in_map_units_minus1')+1)*16
    left=get('frame_crop_left_offset',0)*2
    right=get('frame_crop_right_offset',0)*2
    top=get('frame_crop_top_offset',0)*2
    bottom=get('frame_crop_bottom_offset',0)*2
    assert cw-left-right==width and ch-top-bottom==height,(path.name,cw,ch,left,right,top,bottom)
    assert cw==(width+15)//16*16 and ch==(height+15)//16*16
    if sar==(16,15):
        assert get('aspect_ratio_idc')==255 and get('sar_width')==16 and get('sar_height')==15
    else:
        assert get('aspect_ratio_idc')==1
    stride=(cw+127)//128*128
    return {'coded_width':cw,'coded_height':ch,'visible_width':width,'visible_height':height,
            'crop_left':left,'crop_right':right,'crop_top':top,'crop_bottom':bottom,
            'sar_num':sar[0],'sar_den':sar[1],'matrix_coefficients':matrix,'full_range':int(full),
            'board_stride':stride,'board_uv_offset':stride*ch,'board_total_bytes':stride*ch*3//2,
            'profile_idc':66,'level_idc':31,'ref_frames':1,'cabac':0,'b_frames':0}


def read_exact(handle,n):
    parts=[]
    while n:
        part=handle.read(n)
        if not part: raise RuntimeError('truncated software decode')
        parts.append(part); n-=len(part)
    return b''.join(parts)


def nv12_crcs(ffmpeg,path,reports,width,height,full,native):
    extent=width*height*3//2; ybytes=width*height
    level='full' if full else 'limited'
    command=[ffmpeg,'-v','error','-i',path,'-map','0:v:0','-fps_mode','passthrough',
             '-vf',f'scale=in_range={level}:out_range={level},format=nv12',
             '-pix_fmt','nv12','-f','rawvideo','-']
    rows=[]; decoded_sha=hashlib.sha256()
    with (reports/(path.stem+'.decode.log')).open('wb') as log:
        p=subprocess.Popen(list(map(str,command)),stdout=subprocess.PIPE,stderr=log)
        try:
            for i in range(90):
                frame=read_exact(p.stdout,extent); decoded_sha.update(frame)
                rows.append({'frame':i,'pts_us':i*1000000//30,
                             'y_crc32':f'{zlib.crc32(frame[:ybytes])&0xffffffff:08x}',
                             'uv_crc32':f'{zlib.crc32(frame[ybytes:])&0xffffffff:08x}',
                             'nv12_crc32':f'{zlib.crc32(frame)&0xffffffff:08x}'})
                if i==0:
                    (reports/(path.stem+'.frame000.nv12')).write_bytes(frame)
                    # Verify the encoded luma levels, not just container tags.
                    if native: wx,wy,bx,by=width*3//4,height//16,width*3//4,height*15//16
                    else: wx,wy,bx,by=width//16,height//4,width*15//16,height//4
                    white,black=frame[wy*width+wx],frame[by*width+bx]
                    assert abs(white-(255 if full else 235))<=2,(path.name,'white',white)
                    assert abs(black-(0 if full else 16))<=2,(path.name,'black',black)
            assert p.stdout.read(1)==b'',path.name+' has extra video frames'
        finally: p.stdout.close()
        if p.wait(): raise RuntimeError('decode failed: '+path.name)
    save(reports/(path.stem+'.nv12-crc.json'),{
        'layout':'visible decoded NV12, padding excluded; preserve encoded full/limited levels',
        'width':width,'height':height,'pc_visible_stride':width,'frames':rows,
        'decoded_nv12_stream_sha256':decoded_sha.hexdigest()})
    return {'decoded_frames':len(rows),'visible_nv12_sha256':decoded_sha.hexdigest(),
            'frame000':rows[0],'frame089':rows[-1],
            'crc_report':(reports/(path.stem+'.nv12-crc.json')).name}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg',type=Path,default=ROOT/'tools/ffmpeg/bin/ffmpeg.exe')
    parser.add_argument('--output',type=Path,default=ROOT/'build/player-fixture/geometry-color')
    args=parser.parse_args()
    ffmpeg=args.ffmpeg.resolve(); ffprobe=ffmpeg.with_name('ffprobe'+ffmpeg.suffix)
    output=args.output.resolve()
    for part in ('_sources','_reports','_host'): (output/part).mkdir(parents=True,exist_ok=True)
    reports=output/'_reports'
    helper=load('format_fixture_helpers',ROOT/'scripts/make-player-format-fixtures.py')
    pcm_path=output/'_sources/source-48000-1ch.wav'
    audio=helper.write_source_audio(pcm_path,48000,1,3)
    source,source_info=source_video(ffmpeg,output)
    tests=[]
    for width,height in [(320,240),(640,360),(640,480),(854,480),(960,540),(1280,720),(720,1280)]:
        tests.append((f'geometry-{width}x{height}-601-full',width,height,6,True,(1,1),width==720 and height==1280))
    for matrix,full in [(6,False),(1,False),(1,True)]:
        tests.append((f'color-1280x720-{601 if matrix==6 else 709}-'+('full' if full else 'limited'),
                      1280,720,matrix,full,(1,1),False))
    tests.append(('geometry-720x576-sar16x15-601-limited',720,576,6,False,(16,15),False))
    report={'schema':1,'seconds':3,'fps':30,'status':'building','source_video':source_info,
            'source_audio':audio,'files':[],
            'note':'Isolated PC artifacts only; no board, GUI, CSC hardware, or dimension compatibility claim.'}
    mp4_test=load('mp4_geometry_test',ROOT/'scripts/test-player-mp4.py')
    executable=mp4_test.compile_host(output/'_host','cl')
    ff=[ffmpeg,'-v','error','-y']
    for name,width,height,matrix,full,sar,native in tests:
        path=output/(name+'.mp4')
        color='smpte170m' if matrix==6 else 'bt709'
        level='full' if full else 'limited'
        filtering=('transpose=clock,' if native else '')
        filtering+=f'scale={width}:{height}:out_color_matrix={color}:out_range={level},format=yuv420p,setsar={sar[0]}/{sar[1]}'
        x264='bframes=0:ref=1:cabac=0:keyint=30:min-keyint=30:scenecut=0:open-gop=0:repeat-headers=1:aud=1:force-cfr=1'
        x264+=f':colorprim={color}:transfer={color}:colormatrix={color}:fullrange='+('on' if full else 'off')
        run(ff+['-i',source,'-i',pcm_path,'-map','0:v:0','-map','1:a:0','-vf',filtering,
                '-t','3','-c:v','libx264','-preset','medium','-profile:v','baseline','-level:v','3.1',
                '-threads:v','1','-bf','0','-refs','1','-g','30','-sc_threshold','0','-x264-params',x264,
                '-b:v','2M','-maxrate','3M','-bufsize','3M','-colorspace',color,'-color_primaries',color,
                '-color_trc',color,'-color_range','pc' if full else 'tv','-c:a','aac','-profile:a','aac_low',
                '-b:a','96k','-ar','48000','-ac','1','-threads:a','1','-map_metadata','-1',
                '-fflags','+bitexact','-flags:a','+bitexact','-movflags','+faststart',path])
        meta=json.loads(run([ffprobe,'-v','error','-show_streams','-show_format','-show_packets','-of','json',path]))
        save(reports/(name+'.ffprobe.json'),meta)
        video=next(s for s in meta['streams'] if s['codec_type']=='video')
        aud=next(s for s in meta['streams'] if s['codec_type']=='audio')
        assert video['width']==width and video['height']==height and video['has_b_frames']==0
        assert video['profile']=='Constrained Baseline' and video['level']==31
        assert video['sample_aspect_ratio']==f'{sar[0]}:{sar[1]}'
        assert video['color_space']==color and video['color_range']==('pc' if full else 'tv')
        assert video['r_frame_rate']=='30/1' and video['avg_frame_rate']=='30/1'
        assert int(aud['sample_rate'])==48000 and aud['channels']==1 and aud['profile']=='LC'
        assert int(aud['duration_ts'])==144000 and aud['time_base']=='1/48000'
        geometry=trace_sps(ffmpeg,path,reports/(name+'.trace-headers.log'),width,height,matrix,full,sar)
        crc=nv12_crcs(ffmpeg,path,reports,width,height,full,native)
        native_log=run([executable,path,reports/name]).decode()
        (reports/(name+'.native.jsonl')).write_text(native_log,encoding='utf-8')
        rows=[json.loads(line) for line in native_log.splitlines()]
        assert rows[-1]['done']['video']==90 and rows[0]['info']['audio']['edit_duration']==144000
        maximum=max(row['packet']['normalized'] for row in rows if row.get('packet',{}).get('kind')==1)
        assert maximum<=512*1024
        run(ff+['-i',path,'-frames:v','1',reports/(name+'.preview.png')])
        item={'file':path.name,'bytes':path.stat().st_size,'sha256':sha(path),
              'native_pattern_rotation':'clock90' if native else 'none',**geometry,**crc,
              'max_normalized_au':maximum,'source_audio_frames':144000,
              'container_audio_frames':144000,'native_demux_packets':rows[-1]['done']}
        report['files'].append(item); save(output/'manifest.json',report)
        print(f'PASS {path.name}: coded={geometry["coded_width"]}x{geometry["coded_height"]}, cropR/B={geometry["crop_right"]}/{geometry["crop_bottom"]}, matrix={matrix}, full={int(full)}, NV12CRC90',flush=True)
    report['status']='pc-verified'
    report['copy_files']=[f['file'] for f in report['files']]
    report['total_copy_bytes']=sum(f['bytes'] for f in report['files'])
    save(output/'manifest.json',report)
    lines=['# Step4尺寸与色彩样片','',
           '顶层11个MP4每个3秒、90帧，BP L3.1/B0/ref1/CAVLC、AAC-LC48kmono，PCM源peak8192。',
           '仅复制顶层MP4；_sources/_reports/_host是PC证据。未经板测不能标为兼容通过。','',
           'NV12 CRC按visible宽逐行取Y/UV，排除coded行列padding；参考保留实际full/limited编码样值，绝不把limited扩成full再比CRC。',
           '板端UV起点应为stride*coded_height，再加crop_top/2*stride+crop_left；原生portrait源先clock90，与原AVI预旋转方向一致。',
           '视频原RGB色条/灰阶/四角/帧号源只复用一份；SAR16:15的720x576有效DAR4:3，期望横屏viewport960x720居中。','',
           '|文件|coded|visible|SAR|matrix|full|首帧Y CRC|首帧UV CRC|','|---|---|---|---|---:|---:|---|---|']
    for f in report['files']:
        lines.append(f'|{f["file"]}|{f["coded_width"]}x{f["coded_height"]}|{f["visible_width"]}x{f["visible_height"]}|{f["sar_num"]}:{f["sar_den"]}|{f["matrix_coefficients"]}|{f["full_range"]}|{f["frame000"]["y_crc32"]}|{f["frame000"]["uv_crc32"]}|')
    (output/'README.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print(f'PC VERIFIED: {len(tests)} files, {report["total_copy_bytes"]} media bytes; {output/"manifest.json"}',flush=True)


if __name__=='__main__': main()
