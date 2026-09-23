#!/usr/bin/env python3
"""Build one board resource pack from the supplied watch prototype.

Requires Pillow, Node+the prototype's react/lucide modules, sharp, and FFmpeg.
Windows fonts are read locally; complete font files are never copied.
Example: python generate_watch_resources.py --replace-videos-in watch_ui.pack
  --standby-video standby.mp4 --speaking-video speaking.mp4 --ffmpeg FFMPEG
  --work-dir build/watch-detail-fix
Full generation also requires --prototype PATH --node NODE --sharp SHARP_PACKAGE.
"""
# Lucide ISC License (applies to the rasterized icon shapes in watch_ui.pack):
# Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2022 as part
# of Feather (MIT). All other copyright (c) for Lucide are held by Lucide
# Contributors 2022.
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
# Noto Sans SC glyphs are generated from LVGL's existing test font, licensed
# under SIL Open Font License 1.1. Copyright 2014-2021 Adobe, Reserved Font Name
# 'Source'. The complete notice remains beside the source font in OFL.txt.
# Clock digits use LVGL's existing Montserrat Medium, SIL OFL 1.1:
# Copyright 2011 The Montserrat Project Authors
# (https://github.com/JulietaUla/Montserrat).
from pathlib import Path
import argparse, hashlib, io, json, math, re, struct, subprocess
from PIL import Image, ImageChops, ImageDraw, ImageFont

MAGIC = b'BAJIUI1\0'
COMMON_DIR = Path(__file__).resolve().parents[1]
PROJECT_ROOT = COMMON_DIR.parents[3]
PACK_VERSION = 4  # v4 adds standby/speaking frame counts after the original header.
HEADER = struct.Struct('<8s10I')
CLIPS = struct.Struct('<II')
FONT = struct.Struct('<HBBhhIIII')
GLYPH = struct.Struct('<IIHBBbb')
ICON = struct.Struct('<32sHHII')
FRAME = struct.Struct('<II')
SIZES = [7,8,9,10,11,12,13,14,15,16,19,20,22,24,27,30,33,40,41,42,54]
# CSS uses half-pixel sizes and several real weights. Keep those faces at their
# requested size; rounding 9.5 px to 10 px and treating 600/800 as "bold" changes
# both the stroke and layout of almost every small caption in the prototype.
FONT_SPECS = sorted({(size, weight) for size in SIZES for weight in (400,700)} | {
    (7.5,400),(8.5,400),(9.5,400),(10.5,400),(11.5,400),(12.5,400),
    (9,600),(9.5,600),(10,600),(10.5,600),(11,600),(12,600),(12.5,600),(13,600),
    (9.5,700),(10.5,700),(11.5,700),(12.5,700),
    (12,800),(14,600),(14,800),(15,800),(24,800),(27,800),(33,800),(41,800),
    (32,400),(44,400),
})
COMMON_FONT_SIZE = 14
DEFAULT_CJK_FONT = PROJECT_ROOT/'managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf'
CLOCK_FONT = PROJECT_ROOT/'managed_components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf'
CLOCK_FONT_SIZES = (32,44)
CLOCK_CHARSET = sorted(map(ord, '-:0123456789'))
# Large faces are mostly clocks/rollers. Keep their actual Chinese headings at
# the requested size instead of silently falling back to the 11 px common face.
# Keys match WatchUi::RenderCalendar and RefreshReminder, plus prototype wording.
LARGE_TEXT = {20: '时间待同步', 24: '时间到了！', 27: '月日'}
EXTRA_ICONS = ['heart','star','refresh-cw','rotate-ccw','rotate-cw','power','trash-2','bell-off','wifi-off','zap','loader','delete']
# The board's shared rows, empty states and control items choose their icons
# through C++ data tables. JSX cannot describe those native display sizes.
# Keep this list paired with WatchUi::Icon calls so these small marks are
# rasterized directly instead of shrinking the 48 px fallback on the LCD.
BOARD_ICON_SIZES = {
    16: ('alarm-clock','calendar','check','delete','lightbulb','lock','settings'),
    18: ('arrow-left','check','chevron-right','download','eye','eye-off','info',
         'languages','lock','moon','play','plus','refresh-cw','rotate-ccw',
         'signal','star','sun','trash-2','wifi','x'),
    20: ('alarm-clock','battery','calendar-days','flashlight','heart','info',
         'message-circle-heart','mic','moon','pause','power','rotate-cw',
         'signal','sun','timer','volume-2','volume-x','wifi'),
}


def run(args):
    subprocess.run([str(arg) for arg in args],check=True)


def chat_shading():
    # Match the prototype's static overlays without adding any video motion:
    # radial transparent 52%, black 28% at 72%, black 70% at 100%; bottom 30%
    # fades from transparent to black 65%. Only the video is shaded, not text.
    gain=[]
    for y in range(360):
        bottom=max(0., (y-252)/107)*.65
        for x in range(360):
            radius=math.hypot(x-179.5,y-179.5)/254.5584
            vignette=0 if radius<.52 else (radius-.52)*1.4 if radius<.72 else .28+(radius-.72)*1.5
            gain.append(round(255*(1-min(1.,vignette))*(1-bottom)))
    mask=Image.new('L',(360,360));mask.putdata(gain)
    return mask.convert('RGB')


def probe_video(ffmpeg, source):
    # FFmpeg-only installations (including imageio-ffmpeg) need no ffprobe.
    result=subprocess.run([str(ffmpeg),'-hide_banner','-i',str(source),'-map','0:v:0',
                           '-frames:v','1','-an','-f','null','-'],capture_output=True,
                          text=True,encoding='utf-8',errors='replace',check=True)
    stream=next((line for line in result.stderr.splitlines()
                 if 'Stream #0:' in line and 'Video:' in line), '')
    dimensions=re.search(r'(?<![\d.])(\d{2,5})x(\d{2,5})(?!\d)',stream)
    if not dimensions:raise ValueError(f'Cannot determine video dimensions: {source}')
    rate=re.search(r'([\d.]+) fps',stream)
    duration=re.search(r'Duration: (\d+):(\d+):([\d.]+)',result.stderr)
    return {'width':int(dimensions[1]),'height':int(dimensions[2]),
            'source_fps':float(rate[1]) if rate else None,
            'source_duration_seconds':int(duration[1])*3600+int(duration[2])*60+float(duration[3]) if duration else None,
            'source_audio_streams':sum('Stream #0:' in line and 'Audio:' in line for line in result.stderr.splitlines())}


def video_frames(ffmpeg, source, mjpeg, fps=10, media=None):
    media=media or probe_video(ffmpeg,source)
    # Square 360 px input is left on its original pixel grid. Other input is
    # scaled uniformly and center-cropped; never add zoom/breathing/translation.
    filters=[]
    if (media['width'],media['height'])!=(360,360):
        filters+=['scale=360:360:force_original_aspect_ratio=increase:flags=lanczos',
                  'crop=360:360:(iw-360)/2:(ih-360)/2']
    filters += [f'fps={fps}','setsar=1']
    command=[str(ffmpeg),'-hide_banner','-loglevel','error','-i',str(source),
             '-map','0:v:0','-vf',','.join(filters),
             '-an','-sn','-dn','-pix_fmt','rgb24','-f','rawvideo','pipe:1']
    encode_command=[str(ffmpeg),'-hide_banner','-loglevel','error','-f','rawvideo',
             '-pixel_format','rgb24','-video_size','360x360','-framerate',str(fps),'-i','pipe:0',
             '-an','-pix_fmt','yuvj420p','-c:v','mjpeg','-q:v','4','-f','mjpeg','-y',str(mjpeg)]
    frame_count=0;frame_bytes=360*360*3;shading=chat_shading()
    with subprocess.Popen(command,stdout=subprocess.PIPE) as process, subprocess.Popen(encode_command,stdin=subprocess.PIPE) as encoder:
        while True:
            raw=process.stdout.read(frame_bytes)
            if not raw:break
            assert len(raw)==frame_bytes
            image=Image.frombytes('RGB',(360,360),raw)
            image=ImageChops.multiply(image,shading)
            encoder.stdin.write(image.tobytes());frame_count+=1
        assert process.wait()==0
        encoder.stdin.close()
        assert encoder.wait()==0
    encoded=mjpeg.read_bytes();frames=[];pos=0
    while pos<len(encoded):
        assert encoded[pos:pos+2]==b'\xff\xd8'
        end=encoded.find(b'\xff\xd9',pos+2)+2
        assert end>pos+2
        frames.append(encoded[pos:end]);pos=end
    assert len(frames)==frame_count
    return frames


def read_ui_assets(data):
    if len(data)<HEADER.size:raise ValueError('Resource header is truncated')
    header=HEADER.unpack_from(data)
    _,version,total,nfonts,nicons,nframes,fps,font_at,icon_at,frame_at,video_at=header
    if header[0]!=MAGIC or version not in (1,2,3,4) or total!=len(data):
        raise ValueError('Invalid resource header')
    first_table=HEADER.size+(CLIPS.size if version==4 else 0)
    if not (0<nfonts<=100 and 0<nicons<=256 and 0<nframes<=1000 and 1<=fps<=30):
        raise ValueError('Invalid resource counts or frame rate')
    if not (first_table<=font_at and font_at+nfonts*FONT.size<=icon_at and
            icon_at+nicons*ICON.size<=frame_at and frame_at+nframes*FRAME.size<=video_at<=total):
        raise ValueError('Invalid resource table offsets')
    if version==4:
        standby,speaking=CLIPS.unpack_from(data,HEADER.size)
        if not standby or not speaking or standby+speaking!=nframes:
            raise ValueError('Invalid video clip counts')
    payload_start=frame_at+nframes*FRAME.size
    def payload(offset,size):
        if offset<payload_start or size<0 or offset+size>video_at:
            raise ValueError('UI payload points outside the UI region')
        return data[offset:offset+size]
    fonts=[];icons=[]
    for i in range(nfonts):
        fields=FONT.unpack_from(data,font_at+i*FONT.size)
        glyphs=payload(fields[6],fields[5]*GLYPH.size)
        bitmap=payload(fields[7],fields[8])
        fonts.append((fields,glyphs,bitmap))
    for i in range(nicons):
        fields=ICON.unpack_from(data,icon_at+i*ICON.size)
        if fields[1]*fields[2]!=fields[4]:raise ValueError('Invalid icon pixel size')
        icons.append((fields,payload(fields[3],fields[4])))
    return header,fonts,icons


def make_pack(fonts,icons,clips,fps):
    if len(clips)!=2 or not all(clips):raise ValueError('Both video clips must contain frames')
    frames=clips[0]+clips[1]
    if len(frames)>1000:raise ValueError('Combined videos exceed the device limit of 1000 frames')
    font_at=HEADER.size+CLIPS.size
    icon_at=font_at+len(fonts)*FONT.size
    frame_at=icon_at+len(icons)*ICON.size
    pack=bytearray(frame_at+len(frames)*FRAME.size)
    def append(blob):
        while len(pack)%4:pack.append(0)
        offset=len(pack);pack.extend(blob);return offset
    for i,(fields,glyphs,bitmap) in enumerate(fonts):
        fields=list(fields)
        fields[6]=append(glyphs);fields[7]=append(bitmap)
        FONT.pack_into(pack,font_at+i*FONT.size,*fields)
    for i,(fields,pixels) in enumerate(icons):
        fields=list(fields);fields[3]=append(pixels)
        ICON.pack_into(pack,icon_at+i*ICON.size,*fields)
    while len(pack)%4:pack.append(0)
    video_at=len(pack)
    for i,frame in enumerate(frames):
        FRAME.pack_into(pack,frame_at+i*FRAME.size,append(frame),len(frame))
    HEADER.pack_into(pack,0,MAGIC,PACK_VERSION,len(pack),len(fonts),len(icons),len(frames),fps,
                     font_at,icon_at,frame_at,video_at)
    CLIPS.pack_into(pack,HEADER.size,len(clips[0]),len(clips[1]))
    return bytes(pack)


def validate_videos(pack):
    header,_,_=read_ui_assets(pack)
    if header[1]!=PACK_VERSION:raise ValueError('Video validation requires a v4 pack')
    counts=CLIPS.unpack_from(pack,HEADER.size)
    sizes=[];last_end=header[10]
    for i in range(header[5]):
        offset,size=FRAME.unpack_from(pack,header[9]+i*FRAME.size)
        if offset<last_end or offset-last_end>3 or offset+size>len(pack):
            raise ValueError('Invalid video frame range')
        with Image.open(io.BytesIO(pack[offset:offset+size])) as image:
            image.load()
            if image.format!='JPEG' or image.size!=(360,360) or image.mode!='RGB' or image.info.get('progressive'):
                raise ValueError('Device frames must be baseline 360x360 RGB JPEG')
        sizes.append(size);last_end=offset+size
    if last_end!=len(pack):raise ValueError('Unreferenced bytes remain after the final frame')
    return {'decoded_frames':len(sizes),'standby_frames':counts[0],'speaking_frames':counts[1],
            'max_frame_bytes':max(sizes),'fps':header[6],'frame_width':360,'frame_height':360,
            'audio_streams':0,'bytes':len(pack),'ui_bytes':header[10],
            'video_bytes':len(pack)-header[10]}


def verify_ui_unchanged(before,after):
    old,oldfonts,oldicons=read_ui_assets(before)
    new,newfonts,newicons=read_ui_assets(after)
    if old[3:5]!=new[3:5]:raise ValueError('Font/icon count changed during video replacement')
    digest=hashlib.sha256();glyph_count=0
    for (a,ag,ab),(b,bg,bb) in zip(oldfonts,newfonts):
        if a[:6]+a[8:]!=b[:6]+b[8:] or ag!=bg or ab!=bb:
            raise ValueError('Font descriptors or pixels changed during video replacement')
        digest.update(ag);digest.update(ab);glyph_count+=a[5]
    for (a,ap),(b,bp) in zip(oldicons,newicons):
        if a[:3]+a[4:]!=b[:3]+b[4:] or ap!=bp:
            raise ValueError('Icon metadata or pixels changed during video replacement')
        digest.update(ap)
    return {'fonts_unchanged':len(oldfonts),'glyphs_unchanged':glyph_count,
            'icons_unchanged':len(oldicons),'ui_payload_sha256':digest.hexdigest()}


def convert_clips(args,work):
    clips=[];reports=[]
    for name,source in (('standby',args.standby_video),('speaking',args.speaking_video)):
        media=probe_video(args.ffmpeg,source)
        frames=video_frames(args.ffmpeg,source,work/f'{name}-{args.video_fps}fps.mjpg',args.video_fps,media)
        if not frames:raise ValueError(f'Video clip has no frames: {source}')
        clips.append(frames)
        reports.append({'name':name,'source':str(source.resolve()),
                        'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),**media,
                        'frames':len(frames),'fps':args.video_fps,'duration_seconds':len(frames)/args.video_fps,
                        'jpeg_bytes':sum(map(len,frames)),'max_frame_bytes':max(map(len,frames)),
                        'jpeg_payload_sha256':hashlib.sha256(b''.join(frames)).hexdigest(),
                        'geometry':'unchanged 360x360' if (media['width'],media['height'])==(360,360) else 'center cover',
                        'extra_motion':False,'static_chat_shading':True,'output_audio_streams':0})
    return clips,reports


def save_pack(args,work,pack,report):
    report.update(validate_videos(pack))
    report.update({'version':PACK_VERSION,'video_includes_shading':True,
                   'pack_sha256':hashlib.sha256(pack).hexdigest()})
    if report['bytes']>8*1024*1024:raise ValueError('Pack alone exceeds the 8 MiB assets partition')
    if report.get('estimated_assets_bytes',0)>8*1024*1024:
        raise ValueError('New resources exceed the 8 MiB assets partition')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(pack)
    (work/'manifest.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('fonts','icons')},indent=2))


def font_data(size, weight, charset, fonts_dir, cjk_font=DEFAULT_CJK_FONT):
    # Match Windows' CSS font-weight selection for the prototype's system-ui
    # family: 600 has a real Semibold face; 800 selects the next heavier face
    # (Segoe UI Black/900). Small Chinese remains Regular: bold strokes at
    # 10-12 px merge inside complicated characters on the physical LCD.
    # Rasterize at the final size, with FreeType hinting. Supersampling followed
    # by downscaling would soften the already very small Chinese strokes.
    latin_name = {400:'segoeui.ttf',600:'seguisb.ttf',700:'segoeuib.ttf',800:'seguibl.ttf'}[weight]
    # The BAJI-style menu clock owns two distinct faces. Existing UI faces
    # retain their previous Segoe/Noto sources and exact rasterization.
    clock_face = weight == 400 and size in CLOCK_FONT_SIZES
    latin = ImageFont.truetype(str(CLOCK_FONT if clock_face else fonts_dir/latin_name), size)
    symbols = ImageFont.truetype(str(fonts_dir/'seguisym.ttf'),size)
    # Noto uses the same source family as XiaoZhi. Use an integer CJK pixel
    # grid and ordinary grayscale hinting; no synthetic bold or resizing.
    chinese = ImageFont.truetype(str(cjk_font), math.floor(size+.5))
    ascent,descent = latin.getmetrics()
    # Latin-only clock/keypad faces retain their existing baseline. Noto's
    # taller CJK metrics should only enter a face that actually contains CJK.
    if any(cp>=0x3000 for cp in charset):
        ascent=max(ascent,chinese.getmetrics()[0])
        descent=max(descent,chinese.getmetrics()[1])
    bitmap=bytearray(); glyphs=[]
    for cp in charset:
        font=symbols if cp in (0x21e7,0x2726) else latin if cp<0x3000 else chinese
        ch=chr(cp);bbox=font.getbbox(ch,anchor='ls');x0,y0,x1,y1=bbox
        w,h=x1-x0,y1-y0
        advance=round(font.getlength(ch)*16)
        assert 0<=advance<4096 and w<256 and h<256 and -128<=x0<=127 and -128<=-y1<=127
        glyphs.append((cp,len(bitmap),advance,w,h,x0,-y1))
        if w*h:
            image=Image.new('L',(w,h));ImageDraw.Draw(image).text((-x0,-y0),ch,font=font,fill=255,anchor='ls')
            pixels=list(image.get_flattened_data() if hasattr(image,'get_flattened_data') else image.getdata())
            for index in range(0,len(pixels),2):
                a=(pixels[index]+8)//17
                b=(pixels[index+1]+8)//17 if index+1<len(pixels) else 0
                bitmap.append((min(a,15)<<4)|min(b,15))
    assert len(bitmap)<0x100000
    return ascent+descent,descent,glyphs,bitmap


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prototype',type=Path)
    parser.add_argument('--node',type=Path)
    parser.add_argument('--sharp',type=Path)
    parser.add_argument('--replace-videos-in',type=Path,
                        help='Reuse this pack\'s font/icon payloads and replace both video clips only')
    parser.add_argument('--standby-video',type=Path,required=True)
    parser.add_argument('--speaking-video',type=Path,required=True)
    parser.add_argument('--video-fps',type=int,default=10,
                        help='Common playback rate, 1..30 fps (default: source assets\' native 10 fps)')
    parser.add_argument('--ffmpeg',type=Path,required=True)
    parser.add_argument('--fonts',type=Path,default=Path('C:/Windows/Fonts'))
    parser.add_argument('--cjk-font',type=Path,default=DEFAULT_CJK_FONT,
                        help='Noto Sans SC Regular source; defaults to the existing LVGL test font')
    parser.add_argument('--work-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=COMMON_DIR/'resources/watch_ui.pack')
    parser.add_argument('--common-charset',type=Path,default=PROJECT_ROOT/'managed_components/78__xiaozhi-fonts/charsets/common.json')
    args=parser.parse_args();work=args.work_dir.resolve();work.mkdir(parents=True,exist_ok=True)
    if not 1<=args.video_fps<=30:parser.error('--video-fps must be between 1 and 30')
    for source in (args.standby_video,args.speaking_video):
        if not source.is_file():parser.error(f'Video source missing: {source}')
    if args.replace_videos_in:
        original=args.replace_videos_in.read_bytes()
        old,fonts,icons=read_ui_assets(original)
        clips,clip_report=convert_clips(args,work)
        pack=make_pack(fonts,icons,clips,args.video_fps)
        report={'mode':'replace videos only','clips':clip_report,
                'source_pack_sha256':hashlib.sha256(original).hexdigest(),
                'old_video_frames_removed':old[5],**verify_ui_unchanged(original,pack)}
        assets=PROJECT_ROOT/'build/generated_assets.bin'
        if assets.is_file():
            existing=assets.read_bytes()
            if existing.find(original)>=0:
                other=len(existing)-len(original)
                report.update({'non_pack_assets_bytes':other,'estimated_assets_bytes':other+len(pack),
                               'assets_partition_bytes':8*1024*1024,
                               'estimated_assets_free_bytes':8*1024*1024-other-len(pack)})
        save_pack(args,work,pack,report)
        return
    if not all((args.prototype,args.node,args.sharp)):
        parser.error('Full generation requires --prototype, --node and --sharp')
    if not args.cjk_font.is_file():
        parser.error(f'Noto CJK source missing: {args.cjk_font}; supply --cjk-font explicitly')
    if not CLOCK_FONT.is_file():
        parser.error(f'Montserrat clock source missing: {CLOCK_FONT}')
    prototype=args.prototype.resolve()
    sources=[p for p in (prototype/'src/app').rglob('*.tsx') if p.name not in ('DebugPanels.tsx','DeviceShell.tsx')]
    strings=''.join(p.read_text(encoding='utf-8') for p in sources)
    # Include current board strings, which also cover errors and real settings.
    strings+=''.join(p.read_text(encoding='utf-8') for p in sorted(COMMON_DIR.rglob('*.cc')))
    strings+='专属陪伴随时陪着你'
    ui_chars=sorted(set(range(32,127))|{ord(c) for c in strings if 0xa0<=ord(c)<=0xffef})
    common=set(json.loads(args.common_charset.read_text(encoding='utf-8'))['codepoints'])
    common=sorted(set(ui_chars)|{cp for cp in common if 0x20<=cp<0x10000})
    # The UI promotes all Chinese captions to >=12 px. Tiny faces therefore
    # keep Latin/symbols only, avoiding dozens of unused duplicate CJK sets.
    small_chars=[cp for cp in ui_chars if cp<0x3000]
    imports=set()
    for path in sources:
        for group in re.findall(r'import\s*\{([^}]+)\}\s*from\s*[\'\"]lucide-react',path.read_text(encoding='utf-8'),re.S):
            imports.update(part.strip().split(' as ')[0] for part in group.split(',') if part.strip())
    def icon_name(name):
        return re.sub(r'(?<!^)([A-Z])',r'-\1',name).lower().replace('trash2','trash-2').replace('volume2','volume-2').replace('loader2','loader-2')
    names=sorted({icon_name(name) for name in imports}|set(EXTRA_ICONS))
    # Hand is deliberately omitted: the physical board has no usable IMU.
    names=[name for name in names if name!='hand']
    # Keep exact native pixel sizes and stroke/fill choices from the prototype.
    # The 48 px versions are only fallbacks for dynamic sizes such as reminders.
    specs={name:{'name':name,'key':name,'size':48,'stroke':2,'fill':'none'} for name in names}
    for path in sources:
        # Do not let an enclosing component consume icon={<Sun .../>}.
        for component,attrs in re.findall(r'<([A-Z][A-Za-z0-9]*)\b([^<>]*?)/>',path.read_text(encoding='utf-8'),re.S):
            name=icon_name(component)
            size=re.search(r'\bsize=\{([\d.]+)\}',attrs)
            if name not in names or not size: continue
            size=int(float(size.group(1))+.5)
            stroke=re.search(r'\bstrokeWidth=\{([\d.]+)\}',attrs)
            fill=re.search(r'\bfill="([^"]+)"',attrs)
            alpha=re.search(r'rgba\([^,]+,[^,]+,[^,]+,\s*([\d.]+)\)',fill.group(1)) if fill else None
            fill_color=f'rgba(255,255,255,{alpha.group(1)})' if alpha else 'white' if fill else 'none'
            key=f'{name}@{size}'
            specs[key]={'name':name,'key':key,'size':size,'stroke':float(stroke.group(1)) if stroke else 2,'fill':fill_color}
    # The firmware uses the concise loader name for the prototype Loader2.
    if 'loader-2@24' in specs:
        specs['loader@24']={**specs['loader-2@24'],'key':'loader@24'}
    # PowerOverlay renders its menu icons through a component variable, which
    # the JSX literal-size scan cannot infer. Keep these at their native size.
    for name in ('power','rotate-cw','moon'):
        specs[f'{name}@20']={'name':name,'key':f'{name}@20','size':20,'stroke':2.1,'fill':'none'}
    specs['loader@36']={'name':'loader','key':'loader@36','size':36,'stroke':2.5,'fill':'none'}
    specs['message-circle-heart@20']={'name':'message-circle-heart','key':'message-circle-heart@20','size':20,'stroke':2,'fill':'none'}
    for size,board_names in BOARD_ICON_SIZES.items():
        for name in board_names:
            key=f'{name}@{size}'
            # Preserve a prototype's deliberate stroke/fill when it already
            # provides this exact size, including the power menu's 2.1 stroke.
            specs.setdefault(key,{'name':name,'key':key,'size':size,'stroke':2,'fill':'none'})
    specs=[specs[key] for key in sorted(specs)]
    (work/'icon-specs.json').write_text(json.dumps(specs),encoding='utf-8')
    node_script=r'''
const fs=require('fs'),path=require('path');
const [ref,sharpPath,work]=process.argv.slice(2);const sharp=require(sharpPath);
const rr=require('module').createRequire(path.join(ref,'package.json'));
const React=rr('react'),render=rr('react-dom/server').renderToStaticMarkup,lucide=rr('lucide-react');
const specs=JSON.parse(fs.readFileSync(path.join(work,'icon-specs.json'),'utf8'));
(async()=>{
for(const spec of specs){
 const {name,key:filename,size,stroke,fill}=spec;
 const key=name.split('-').map(s=>s[0].toUpperCase()+s.slice(1)).join('');
 if(!lucide[key])throw new Error('Unknown icon '+key);
 const svg=name==='loader'&&size===36
   ? '<svg xmlns="http://www.w3.org/2000/svg" width="36" height="36" viewBox="0 0 36 36"><path d="M34.75 18 A16.75 16.75 0 0 1 1.25 18" fill="none" stroke="white" stroke-width="2.5"/></svg>'
   : render(React.createElement(lucide[key],{size,color:'white',strokeWidth:stroke,fill}));
 const raw=await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
 const alpha=Buffer.alloc(size*size);for(let i=0;i<alpha.length;i++)alpha[i]=raw[i*4+3];
 fs.writeFileSync(path.join(work,filename+'.a8'),alpha);
}
})();
'''
    (work/'icons.cjs').write_text(node_script,encoding='utf-8')
    run([args.node,work/'icons.cjs',prototype,args.sharp,work])
    clips,clip_report=convert_clips(args,work)
    fonts=[];font_report=[]
    for size,weight in FONT_SPECS:
        charset=CLOCK_CHARSET if weight==400 and size in CLOCK_FONT_SIZES else common if size==COMMON_FONT_SIZE and weight==400 else small_chars if size<12 else ui_chars if size<=16 else sorted(
            set(range(32,127))|{ord(c) for c in LARGE_TEXT.get(size,'')})
        line,base,glyphs,bitmap=font_data(size,weight,charset,args.fonts,args.cjk_font)
        # reserved=1: half-pixel size plus numeric CSS weight / 100. The reader
        # also accepts reserved=0 entries from the original integer/bool pack.
        fields=(round(size*2),weight//100,1,line,base,len(glyphs),0,0,len(bitmap))
        fonts.append((fields,b''.join(GLYPH.pack(*glyph) for glyph in glyphs),bytes(bitmap)))
        font_report.append({'size':size,'weight':weight,'bold':weight>=600,'glyphs':len(glyphs),'bytes':len(glyphs)*GLYPH.size+len(bitmap)})
    icons=[]
    for spec in specs:
        name=spec['key'];size=spec['size']
        pixels=(work/(name+'.a8')).read_bytes()
        icons.append(((name.encode().ljust(32,b'\0'),size,size,0,len(pixels)),pixels))
    pack=make_pack(fonts,icons,clips,args.video_fps)
    report={'mode':'full generation','clips':clip_report,'fonts':font_report,'icons':specs,
            'cjk_family':'Noto Sans SC Regular','cjk_source_sha256':hashlib.sha256(args.cjk_font.read_bytes()).hexdigest(),
            'common_font_size':COMMON_FONT_SIZE,'clock_family':'Montserrat Medium',
            'clock_source_sha256':hashlib.sha256(CLOCK_FONT.read_bytes()).hexdigest()}
    save_pack(args,work,pack,report)

if __name__=='__main__':main()
