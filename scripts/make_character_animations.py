"""Animate the accepted native 64x64 horizontal character with pixel layers.

Pillow only. Source artwork and original apron emblem stay unchanged on disk.
Every firmware frame is 1-bit; orange is only the preview backlight simulation.
Run: python scripts/make_character_animations.py
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageChops
import json
import math
from pixel_font import PixelFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/whale-girl'
OUT.mkdir(parents=True, exist_ok=True)
ACCEPTED = Image.open(OUT / 'source/accepted-screen.png').convert('1')
BASE = ACCEPTED.crop((0, 0, 64, 64))
CN = 'C:/Windows/Fonts/msyh.ttc'
STATUS_FONT = PixelFont(ROOT / 'firmware/flipper/lib/u8g2/u8g2_fonts.c', 'u8g2_font_5x7_tr')
STATE_FONT = PixelFont(ROOT / 'firmware/flipper/lib/u8g2/u8g2_fonts.c', 'u8g2_font_helvB08_tr')
BATTERY_ICON = Image.open(ROOT / 'firmware/flipper/assets/icons/StatusBar/Battery_26x8.png').convert('1')
LOCK_ICON = Image.open(ROOT / 'firmware/flipper/assets/icons/SubGhz/Lock_7x8.png').convert('1')
ATTENTION_ICON = Image.open(ROOT / 'firmware/flipper/assets/icons/StatusBar/Hidden_window_9x8.png').convert('1')
LABEL_FONT = ImageFont.truetype(CN, 18)
SMALL_FONT = ImageFont.truetype(CN, 15)
TITLE_FONT = ImageFont.truetype(CN, 25)
THEME = {'paper': (233,168,63), 'ink': (37,30,19),
         'background': '#192b36', 'text': '#edf4ed', 'muted': '#b3c8d1'}
TICK_MS = 120
# State durations are preview defaults. The device may hold/interrupt each state.
STATES = [
    ('idle', '待机', 24, '待机', '按键说话'),
    ('listening', '倾听', 16, '正在收音', '按键结束'),
    ('thinking', '思考', 24, '等待回答', '请稍候'),
    ('speaking', '说话', 16, '正在回答', '语音播放'),
    ('happy', '开心', 16, '开心', ''),
    ('shy', '害羞', 24, '害羞', ''),
    ('surprised', '惊讶', 12, '惊讶', ''),
    ('wave', '挥手', 16, '打招呼', ''),
    ('sad', '难过', 24, '难过', ''),
    ('angry', '生气', 16, '生气', ''),
    ('sleeping', '困倦', 24, '困倦', ''),
    ('nod', '点头', 16, '任务完成', ''),
]


def translated(im, dx=0, dy=0):
    result = Image.new('1', im.size, 1)
    result.paste(im, (dx, dy))
    return result


def tail_sway(im, angle):
    if not angle:
        return im.copy()
    region = Image.new('1', (64,64), 0)
    points = [(43,58),(49,55),(54,50),(56,44),(61,40),
              (62,40),(62,45),(58,50),(55,55),(50,58),(43,60)]
    ImageDraw.Draw(region).polygon(points, fill=1)
    ink = ImageChops.logical_and(ImageChops.invert(im), region)
    result = im.copy()
    result.paste(1, (0,0), region)
    moved = ink.rotate(angle, Image.Resampling.NEAREST, center=(44,57), fillcolor=0)
    result.paste(0, (0,0), moved)
    return result


def eyes(im, kind='normal', gaze=0):
    if kind == 'normal' and not gaze:
        return
    d = ImageDraw.Draw(im)
    # Confine edits to the original eyes. Bangs and facial outline are preserved.
    for box in [(18,27,23,32),(31,27,35,31)]:
        d.rectangle(box, fill=1)
    if kind == 'normal':
        for box in [(18,27,24,33),(31,27,36,32)]:
            patch = BASE.crop(box)
            im.paste(patch,(box[0]+gaze,box[1]))
    elif kind in ('closed','half'):
        if kind == 'closed':
            d.line([(18,29),(19,30),(22,30)], fill=0)
            d.line([(31,29),(32,30),(35,29)], fill=0)
        else:
            d.line((18,29,22,29), fill=0)
            d.line((31,29,35,29), fill=0)
            d.point((21,30), fill=0)
            d.point((33,30), fill=0)
    elif kind == 'happy':
        d.line([(18,30),(20,28),(22,30)],fill=0)
        d.line([(31,30),(33,28),(35,30)],fill=0)
    elif kind == 'surprised':
        for x in (19,32):
            d.ellipse((x,27,x+3,32),fill=0)
            d.point((x+1,28),fill=1)
    elif kind == 'sad':
        d.line([(18,29),(20,30),(22,30)],fill=0)
        d.line([(31,30),(33,30),(35,29)],fill=0)
        d.point((21,31),fill=0)
        d.point((32,31),fill=0)
    elif kind == 'angry':
        d.line([(18,27),(20,28),(22,29)],fill=0)
        d.line([(31,29),(33,28),(35,27)],fill=0)
        d.rectangle((20,29,22,31),fill=0)
        d.rectangle((31,29,33,31),fill=0)
    elif kind == 'wink':
        d.line([(18,29),(20,30),(22,29)],fill=0)
        im.paste(BASE.crop((31,27,36,32)),(31,27))


def mouth(im, kind):
    d = ImageDraw.Draw(im)
    d.rectangle((25,33,29,36),fill=1)
    if kind == 'closed':
        d.line((26,35,28,35),fill=0)
    elif kind == 'small':
        d.ellipse((26,34,28,36),fill=0)
    elif kind == 'open':
        d.ellipse((25,33,29,36),fill=0)
        d.line((26,33,28,33),fill=1)
    elif kind == 'smile':
        d.line([(25,34),(26,35),(28,35),(29,34)],fill=0)
    elif kind == 'sad':
        d.line([(25,36),(26,35),(28,35),(29,36)],fill=0)
    elif kind == 'o':
        d.ellipse((26,33,29,36),fill=0)
        d.point((27,34),fill=1)


def heart(d, x, y, size=1):
    pattern = ['.##.##.','#######','#######','.#####.','..###..','...#...']
    for r, row in enumerate(pattern):
        for c, bit in enumerate(row):
            if bit == '#':
                d.rectangle((x+c*size,y+r*size,x+(c+1)*size-1,y+(r+1)*size-1),fill=0)


def sparkle(d, x, y):
    d.line((x-2,y,x+2,y),fill=0)
    d.line((x,y-2,x,y+2),fill=0)


def wave_hand(im, offset):
    d = ImageDraw.Draw(im)
    # Sleeve covers the old fist; the new palm pivots above the cuff.
    d.polygon([(22,39),(22,43),(19,45),(15,42),(12,37),
               (16,35),(18,39)],fill=0)
    d.line([(14,38),(16,37),(18,41)],fill=1)
    d.ellipse((11+offset,31,16+offset,36),fill=1,outline=0)
    d.line((12+offset,32,12+offset,34),fill=0)
    d.line((14+offset,31,14+offset,33),fill=0)


def character(state, i, count):
    phase = i / count
    # Integer angles/positions, no subpixel filtering on firmware artwork.
    angle = round(4 * math.sin(phase * math.tau))
    if state == 'happy':
        angle = round(7 * math.sin(phase * math.tau * 2))
    if state in ('sleeping','sad'):
        angle = 0 if state == 'sleeping' else -3
    # The first idle frame is exactly the accepted sprite.
    im = tail_sway(BASE,angle)
    d = ImageDraw.Draw(im)
    blink = {int(count*.67):'half',int(count*.71):'closed',int(count*.75):'half'}
    if state == 'idle':
        if i in blink:
            eyes(im,blink[i])
    elif state == 'listening':
        eyes(im,'normal',1 if count//3 <= i < count*2//3 else 0)
        mouth(im,'closed')
        if i%8 < 4:
            d.line((3,22,3,25),fill=0)
            d.line((5,20,5,23),fill=0)
    elif state == 'thinking':
        eyes(im,'half' if i%10 >= 7 else 'normal',-1 if i%10<7 else 0)
        mouth(im,'closed')
        if i%10<7:
            d.line([(7,10),(8,9),(10,9),(11,10),(11,11),(9,13),(9,14)],fill=0)
            d.point((9,16),fill=0)
    elif state == 'speaking':
        if i in blink:
            eyes(im,blink[i])
        levels = [0,1,2,1,0,2,1,0,1,2,1,0,0,1,2,0]
        mouth(im,['closed','small','open'][levels[i%len(levels)]])
    elif state == 'happy':
        eyes(im,'happy')
        mouth(im,'open' if i%8 < 4 else 'smile')
        if i%8 < 4:
            sparkle(d,7,17)
            sparkle(d,53,8)
    elif state == 'shy':
        eyes(im,'half')
        mouth(im,'smile')
        d.line((18,33,20,34),fill=0)
        d.line((33,33,35,34),fill=0)
        if i%10<7:
            heart(d,52,7)
    elif state == 'surprised':
        eyes(im,'surprised')
        mouth(im,'o')
        if i%6 < 4:
            d.rectangle((55,8,56,13),fill=0)
            d.rectangle((55,15,56,16),fill=0)
    elif state == 'wave':
        eyes(im,'happy' if i%8<6 else 'wink')
        mouth(im,'smile')
        wave_hand(im,[-1,0,1,0][i//2%4])
        if i%8<4:
            d.line((8,29,7,27),fill=0)
            d.line((17,29,19,27),fill=0)
    elif state == 'sad':
        eyes(im,'sad')
        mouth(im,'sad')
        y = 32 + (i//3)%4
        d.line((23,y,23,y+1),fill=0)
    elif state == 'angry':
        eyes(im,'angry')
        mouth(im,'sad' if i%8<4 else 'closed')
        if i%8<6:
            d.line([(52,8),(52,10),(50,10)],fill=0)
            d.line([(55,8),(55,10),(57,10)],fill=0)
            d.line([(50,13),(52,13),(52,15)],fill=0)
            d.line([(55,15),(55,13),(57,13)],fill=0)
    elif state == 'sleeping':
        eyes(im,'closed')
        mouth(im,'small' if i%12<6 else 'closed')
        x,y = 5+(i//8),10-(i//8)
        d.line([(x,y),(x+3,y),(x,y+3),(x+3,y+3)],fill=0)
    elif state == 'nod':
        eyes(im,'closed' if 4 <= i%8 <= 6 else 'normal')
        mouth(im,'smile')
        if 3 <= i%8 <= 6:
            head=im.crop((0,0,64,39))
            d.rectangle((0,0,63,38),fill=1)
            im.paste(head,(0,1))
    if state in ('idle','happy','sleeping'):
        bob = -1 if (state=='happy' and i%8 in (2,3,4)) else 0
        if state in ('idle','sleeping') and count//3 <= i < count*2//3:
            bob=-1
        if bob:
            im=translated(im,dy=bob)
    return im


def screen(sprite, state, i, title, hint, *, enabled=True, linked=True, volume=20,
           muted=False, microphone=True, locked=False, attention=False):
    # Sample values, using the same font, baselines and alignment as the firmware.
    im=Image.new('1',(128,64),1)
    im.paste(sprite,(0,0))
    im.paste(BATTERY_ICON,(100,2))
    d=ImageDraw.Draw(im)
    STATUS_FONT.draw(im,70,9,'12:30')
    STATUS_FONT.draw(im,111,6+STATUS_FONT.ascent//2,'87',align='center')
    d.line((70,13,125,13),fill=0)
    messages={
        'offline':('Offline','AI settings'), 'setup':('Setup','AI settings'),
        'connecting':('Connecting','Please wait'), 'idle':('Ready','Press TALK'),
        'listening':('Listening','Click: send'), 'thinking':('Thinking','Please wait'),
        'speaking':('Speaking','Voice reply'), 'executing':('NFC scan','Hold a card'),
        'error':('Error','AI settings')}
    status,hint=messages.get(state,messages['idle'])
    if not enabled:
        status,hint='AI off','Local mode'
    elif not linked:
        status,hint='No link','Check ESP32'
    elif state=='idle' and not microphone:
        status,hint='Mic off','AI settings'
    elif state=='speaking' and (muted or volume==0):
        status,hint='Reply','Audio muted'
    STATE_FONT.draw(im,70,27,status)
    STATUS_FONT.draw(im,70,38,hint)
    d.line((70,43,125,43),fill=0)
    audio_known=enabled and linked
    volume_text=('Muted' if muted or volume==0 else f'{volume}%') if audio_known else '--'
    mic_text=('On' if microphone else 'Off') if audio_known else '--'
    for baseline,label,value in ((53,'VOL',volume_text),(63,'MIC',mic_text)):
        STATUS_FONT.draw(im,70,baseline,label)
        STATUS_FONT.draw(im,126,baseline,value,align='right')
    if locked:
        im.paste(LOCK_ICON,(90,55))
    if attention:
        im.paste(ATTENTION_ICON,(90,45))
    return im


def lcd(im,scale=4):
    rgb=Image.new('RGB',im.size)
    rgb.putdata([THEME['paper'] if p else THEME['ink'] for p in im.getdata()])
    return rgb.resize((im.width*scale,im.height*scale),Image.Resampling.NEAREST)


def gif(frames,path,duration=TICK_MS):
    frames[0].save(path,save_all=True,append_images=frames[1:],duration=duration,
                   loop=0,optimize=False,disposal=2)


def pack(im):
    return bytes(sum((im.getpixel((x+b,y))==0)<<b for b in range(8))
                 for y in range(im.height) for x in range(0,im.width,8))


SPRITES={}
SCREENS={}
catalog=[]
unique=[]
lookup={}
for state,label,count,title,hint in STATES:
    folder=OUT/'frames'/state
    folder.mkdir(parents=True,exist_ok=True)
    SPRITES[state]=[]
    SCREENS[state]=[]
    sequence=[]
    for i in range(count):
        sprite=character(state,i,count)
        SPRITES[state].append(sprite)
        frame=screen(sprite,state,i,title,hint)
        SCREENS[state].append(frame)
        sprite.save(folder/f'{i:02d}.png')
        data=pack(sprite)
        if data not in lookup:
            lookup[data]=len(unique)
            unique.append(data)
        sequence.append(lookup[data])
    gif(SPRITES[state],OUT/f'{state}_native.gif')
    gif([lcd(f,4) for f in SCREENS[state]],OUT/f'{state}_screen.gif')
    catalog.append({'id':state,'label':label,'tick_ms':TICK_MS,
                    'loop':True,'preview_status':[title,hint],
                    'frames':sequence})

# Four columns of actual 128x64 screen previews, each enlarged 3x.
def overview(i, static=False):
    w,h=4*408+24,3*240+96
    sheet=Image.new('RGB',(w,h),THEME['background'])
    d=ImageDraw.Draw(sheet)
    d.text((24,12),'横屏鲸鱼娘 · 12 组动作与表情',font=TITLE_FONT,fill=THEME['text'])
    d.text((24,50),'沿用选定造型与围裙鲸鱼；每格实际 128×64 单色，放大 3 倍。',font=SMALL_FONT,fill=THEME['muted'])
    picks={'idle':17,'listening':6,'thinking':9,'speaking':2,'happy':3,
           'shy':3,'surprised':2,'wave':3,'sad':8,'angry':3,'sleeping':9,'nod':5}
    for k,(state,label,count,_,_) in enumerate(STATES):
        x,y=24+(k%4)*408,88+(k//4)*240
        d.text((x,y),label,font=LABEL_FONT,fill=THEME['text'])
        n=picks[state] if static else i%count
        sheet.paste(lcd(SCREENS[state][n],3),(x,y+28))
    return sheet

overview(0,True).save(OUT/'expressions.png')
lcd(screen(BASE,'idle',0,'',''),6).save(OUT/'desktop_preview.png')

# A review sheet covering actual status/permission combinations, not just emotions.
scenes=[
    ('待机','idle','idle',{}),
    ('收音','listening','listening',{}),
    ('思考','thinking','thinking',{}),
    ('播放','speaking','speaking',{}),
    ('AI 关闭','idle','idle',{'enabled':False}),
    ('模块断连','idle','idle',{'linked':False}),
    ('需要配置','idle','setup',{}),
    ('连接中','idle','connecting',{}),
    ('NFC 扫描','thinking','executing',{}),
    ('麦克风关闭','idle','idle',{'microphone':False}),
    ('静音回复','speaking','speaking',{'muted':True}),
    ('错误／锁定／后台提醒','idle','error',{'locked':True,'attention':True,'volume':100}),
]
sheet=Image.new('RGB',(1248,1040),THEME['background'])
sheet_draw=ImageDraw.Draw(sheet)
sheet_draw.text((24,12),'桌面信息规划 · 12 种状态',font=TITLE_FONT,fill=THEME['text'])
sheet_draw.text((24,50),'角色原尺寸；当前阶段 + 操作提示；底部音量 / 麦克风开关。示例数值，非实机截图。',
                font=SMALL_FONT,fill=THEME['muted'])
for k,(label,animation,state,options) in enumerate(scenes):
    x,y=24+(k%3)*408,88+(k//3)*236
    sheet_draw.text((x,y),label,font=LABEL_FONT,fill=THEME['text'])
    preview=screen(SPRITES[animation][0],state,0,'','',**options)
    sheet.paste(lcd(preview,3),(x,y+28))
sheet.save(OUT/'desktop_states.png')
gif([overview(i) for i in range(48)],OUT/'animated_overview.gif')


def character_overview(i):
    sheet=Image.new('RGB',(888,808),THEME['background'])
    d=ImageDraw.Draw(sheet)
    d.text((24,12),'鲸鱼娘 · 动作与表情',font=TITLE_FONT,fill=THEME['text'])
    d.text((24,50),'每个角色为 64×64 单色，放大 3 倍；左侧角色，右侧时间、电量与 AI 状态。',
           font=SMALL_FONT,fill=THEME['muted'])
    for k,(state,label,count,_,_) in enumerate(STATES):
        x,y=24+(k%4)*216,88+(k//4)*236
        d.text((x,y),label,font=LABEL_FONT,fill=THEME['text'])
        sheet.paste(lcd(SPRITES[state][i%count],3),(x,y+28))
    return sheet


character_overview(3).save(OUT/'characters.png')
gif([character_overview(i) for i in range(48)],OUT/'animated_characters.gif')

# A continuous demonstration; all text/audio meter motion here is scripted.
demo=[]
native_demo=[]
for state in ('idle','wave','listening','thinking','speaking','happy','shy','sleeping'):
    for i in range(24):
        frame=SCREENS[state][i%len(SCREENS[state])]
        native_demo.append(frame)
        demo.append(lcd(frame,5))
gif(demo,OUT/'conversation_demo.gif')
gif(native_demo,OUT/'conversation_native_128x64.gif')

# Six mouths for real-time state/emotion composition later.
for kind in ('closed','small','open','smile','sad','o'):
    im=BASE.copy()
    mouth(im,kind)
    im.save(OUT/f'mouth_{kind}.png')
    data=pack(im)
    if data not in lookup:
        lookup[data]=len(unique)
        unique.append(data)

(OUT/'whale_frames.bin').write_bytes(b''.join(unique))
manifest={'version':1,'width':64,'height':64,'bpp':1,
          'packing':'row-major, LSB-first, black=1','bytes_per_frame':512,
          'unique_frame_count':len(unique),'animations':catalog,
          'mouth_frames':{kind:lookup[pack(Image.open(OUT/f'mouth_{kind}.png'))]
                          for kind in ('closed','small','open','smile','sad','o')},
          'source':'source/accepted-screen.png',
          'note':'Preview timing is scripted; hardware validation is pending. UI font is host-rasterized.'}
(OUT/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

header=['// Generated by scripts/make_character_animations.py. Black=1; LSB-first.',
        '#pragma once','#include <stdint.h>','#include <stddef.h>',
        '#define WHALE_FRAME_WIDTH 64','#define WHALE_FRAME_HEIGHT 64',
        '#define WHALE_FRAME_BYTES 512',f'#define WHALE_FRAME_COUNT {len(unique)}',
        'static const uint8_t whale_frames[WHALE_FRAME_COUNT][WHALE_FRAME_BYTES] = {']
for idx,data in enumerate(unique):
    header.append(f'    {{ // frame {idx}')
    header.extend('        '+', '.join(f'0x{x:02x}' for x in data[i:i+16])+',' for i in range(0,512,16))
    header.append('    },')
header+=['};','typedef struct {', '    const char* id;',
         '    const uint16_t* frames;','    uint16_t count;',
         '    uint16_t tick_ms;','} WhaleAnimation;']
for entry in catalog:
    header.append(f'static const uint16_t whale_seq_{entry["id"]}[] = {{'+
                  ', '.join(str(x) for x in entry['frames'])+'};')
header.append('static const WhaleAnimation whale_animations[] = {')
for entry in catalog:
    header.append(f'    {{"{entry["id"]}", whale_seq_{entry["id"]}, {len(entry["frames"])}, {TICK_MS}}},')
header+=['};',f'#define WHALE_ANIMATION_COUNT {len(catalog)}']
for kind,index in manifest['mouth_frames'].items():
    header.append(f'#define WHALE_MOUTH_{kind.upper()} {index}')
(OUT/'whale_animations.h').write_text('\n'.join(header)+'\n',encoding='ascii')
print(f'Created {len(catalog)} animations, {sum(len(x["frames"]) for x in catalog)} sequence frames, '
      f'{len(unique)} unique 512-byte frames ({len(unique)*512} bytes).')
