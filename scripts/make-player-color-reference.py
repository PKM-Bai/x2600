"""Generate PC SDR pixel references from the verified geometry fixtures; no board access."""
from pathlib import Path
import argparse
import json
import subprocess

ROOT = Path(__file__).resolve().parents[1]
COEFFICIENTS = {
    (6,1):(0x400,0x59c,0x160,0x2db,0x717,0),
    (6,0):(0x4a8,0x662,0x191,0x340,0x812,16),
    (1,1):(0x400,0x64d,0xc0,0x1df,0x76c,0),
    (1,0):(0x4a8,0x72c,0xda,0x222,0x873,16),
}


def clamp(v):
    return max(0,min(255,int(v)))


def viewport(w,h,sn,sd):
    n,d=w*sn,h*sd
    cw,ch=(1280,720) if n>=d else (720,1280)
    if n*ch>=d*cw: vw,vh=cw,(cw*d//n)&~1
    else: vw,vh=(ch*n//d)&~1,ch
    return cw,ch,(cw-vw)//2,(ch-vh)//2,vw,vh


def expected_rgb(y,u,v,matrix,full):
    kr,kb=(0.299,0.114) if matrix==6 else (0.2126,0.0722)
    kg=1-kr-kb
    yy=y if full else (y-16)*255/219
    uu=(u-128) if full else (u-128)*255/224
    vv=(v-128) if full else (v-128)*255/224
    ideal=(yy+2*(1-kr)*vv,
           yy-2*kb*(1-kb)/kg*uu-2*kr*(1-kr)/kg*vv,
           yy+2*(1-kb)*uu)
    yc,rv,gu,gv,bu,sub=COEFFICIENTS[matrix,full]
    q10=(yc*(y-sub)+rv*(v-128),
         yc*(y-sub)-gu*(u-128)-gv*(v-128),
         yc*(y-sub)+bu*(u-128))
    return [clamp(round(c)) for c in ideal], [clamp((c+512)//1024) for c in q10]


def offset(x,y,model,height=720):
    if model=='column512': return ((x//128)*height+y)*512+(x%128)*4
    return (y*1280+x)*4


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--fixtures',type=Path,default=ROOT/'build/player-fixture/geometry-color')
    parser.add_argument('--output',type=Path,default=ROOT/'build/player-fixture/geometry-color/_reports/display-samples.json')
    parser.add_argument('--ffmpeg',type=Path,default=ROOT/'tools/ffmpeg/bin/ffmpeg.exe')
    args=parser.parse_args()
    manifest=json.loads((args.fixtures/'manifest.json').read_text(encoding='utf-8'))
    reports=[]
    for item in manifest['files']:
        w,h=item['visible_width'],item['visible_height']
        cw,ch,vx,vy,vw,vh=viewport(w,h,item['sar_num'],item['sar_den'])
        if cw!=1280:
            reports.append({'file':item['file'],'status':'native-no-custom-writeback'})
            continue
        frame000=(args.fixtures/'_reports'/(Path(item['file']).stem+'.frame000.nv12')).read_bytes()
        level='full' if item['full_range'] else 'limited'
        nv12=subprocess.run([str(args.ffmpeg),'-v','error','-i',str(args.fixtures/item['file']),
            '-map','0:v:0','-vf',r'select=eq(n\,89),'+f'scale=in_range={level}:out_range={level},format=nv12',
            '-frames:v','1','-pix_fmt','nv12','-f','rawvideo','-'],capture_output=True,check=True).stdout
        assert len(nv12)==w*h*3//2
        # Eight color centers plus eight stable gray patches outside the moving
        # digits/corner marker and playback GUI. All are far from scaler edges.
        points=[('bar'+str(i),vx+vw*(2*i+1)//16,vy+vh//4) for i in range(8)]
        gray=(0,1,2,3,4,5) if vx else (0,1,2,3,4,5,10,11)
        # Keep gray points clear of the future lower-corner markers and GUI.
        points += [('gray'+str(i),vx+vw*(2*i+1)//32,vy+vh*5//6) for i in gray]
        if vx: points += [('margin-left',vx//2,180),('margin-right',1280-vx//2,180)]
        samples=[]
        for name,x,y in points:
            if name.startswith('margin-'):
                samples.append({'name':name,'x':x,'y':y,'expected_rgb':[0,0,0],
                                'q10_rgb':[0,0,0],'tolerance_per_channel':2,
                                'offset_custom_column512':offset(x,y,'column512'),
                                'offset_linear':offset(x,y,'linear')})
                continue
            sx=min(w-1,(x-vx)*w//vw); sy=min(h-1,(y-vy)*h//vh)
            yy=nv12[sy*w+sx]
            uv=w*h+(sy//2)*w+(sx&~1)
            uu,vv=nv12[uv:uv+2]
            ideal,q10=expected_rgb(yy,uu,vv,item['matrix_coefficients'],item['full_range'])
            if name.startswith('gray'):
                neighborhood=[]
                for ny in range(max(0,sy-1),min(h,sy+2)):
                    for nx in range(max(0,sx-1),min(w,sx+2)):
                        nuv=w*h+(ny//2)*w+(nx&~1)
                        neighborhood.append(expected_rgb(nv12[ny*w+nx],*nv12[nuv:nuv+2],
                            item['matrix_coefficients'],item['full_range'])[0])
                spread=[max(pixel[c] for pixel in neighborhood)-min(pixel[c] for pixel in neighborhood)
                        for c in range(3)]
                if max(spread)>2:
                    raise RuntimeError(f"unstable gray reference {item['file']} {name}: {spread}")
            samples.append({'name':name,'x':x,'y':y,'source_x':sx,'source_y':sy,
                            'yuv':[yy,uu,vv],'expected_rgb':ideal,'q10_rgb':q10,
                            'frame000_yuv':[frame000[sy*w+sx],*frame000[uv:uv+2]],
                            'tolerance_per_channel':2,
                            'offset_custom_column512':offset(x,y,'column512'),
                            'offset_linear':offset(x,y,'linear')})
        reports.append({'file':item['file'],'status':'expected-only-await-hardware',
                        'matrix':item['matrix_coefficients'],'full_range':item['full_range'],
                        'viewport':[vx,vy,vw,vh],'points':samples})
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'schema':1,'custom_mapping_format':'column512',
        'custom_mapping_status':'layout-described-rgb-expected-only',
        'sample_source':'PC decoded NV12 frame089 with preserved encoded range; frame000 samples retained for comparison',
        'argb_interpretation':'compare RGB24, ignore alpha until hardware output packing confirmed',
        'files':reports},indent=2)+'\n',encoding='utf-8')
    print('PASS generated',sum('points' in r for r in reports),'landscape expected sets;',
          sum('points' not in r for r in reports),'native skip; no hardware-pass claim')


if __name__=='__main__': main()
