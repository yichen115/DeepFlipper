"""Draw the pet's native 1-bit expressions from the accepted editable pixel art.

The user's rice illustration is a personality/reference image, not a replacement
for the established 64x64 sprite. No generated illustration is downsampled.
"""
from pathlib import Path
import json
import math
from PIL import Image, ImageDraw, ImageFont, ImageChops
from make_pet_room_concepts import app_room, opacity_mask, pack_mask
from make_pet_room_concepts import pack as pack_wide

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/whale-pet'
APP = ROOT / 'firmware/flipper/applications/examples/whale_pet'
BASE = Image.open(ROOT / 'assets/whale-girl/source/accepted-screen.png').convert('1').crop((0, 0, 64, 64))
TICK = 100
CLIPS = [
    ('idle', '自在待机', 96, True), ('content', '满足', 32, True),
    ('curious', '好奇', 36, True), ('excited', '期待', 32, True),
    ('shy', '害羞', 36, True), ('surprised', '惊讶', 24, False),
    ('proud', '得意', 32, True), ('worried', '担心', 36, True),
    ('comfort', '安慰你', 40, False), ('sleepy', '困了', 48, True),
    ('sleeping', '睡觉', 64, True), ('pat', '被摸摸', 36, False),
    ('rice', '吃白米饭', 48, False), ('play', '玩球', 40, False),
    ('wave', '欢迎回来', 32, False), ('listening', '认真听', 48, True),
    ('thinking', '想一想', 48, True), ('speaking', '说话', 48, True),
    ('full', '吃饱啦', 32, False),
]


def eyes(im, kind='normal', gaze=0):
    if kind == 'normal' and not gaze:
        return
    draw = ImageDraw.Draw(im)
    for box in ((18, 27, 23, 32), (31, 27, 35, 31)):
        draw.rectangle(box, fill=1)
    if kind == 'normal':
        for box in ((18, 27, 24, 33), (31, 27, 36, 32)):
            im.paste(BASE.crop(box), (box[0] + gaze, box[1]))
    elif kind in ('closed', 'half'):
        for x in (18, 31):
            draw.line([(x, 29), (x + 1, 30 if kind == 'closed' else 29), (x + 4, 29)], fill=0)
            if kind == 'half':
                draw.point((x + 2, 30), fill=0)
    elif kind == 'happy':
        for x in (18, 31):
            draw.line([(x, 30), (x + 2, 28), (x + 4, 30)], fill=0)
    elif kind == 'wide':
        for x in (19, 32):
            draw.ellipse((x, 27, x + 3, 32), fill=0)
            draw.point((x + 1, 28), fill=1)
    elif kind == 'worried':
        draw.line([(18, 28), (20, 29), (22, 29)], fill=0)
        draw.line([(31, 29), (33, 29), (35, 28)], fill=0)
        draw.point((21, 31), fill=0)
        draw.point((32, 31), fill=0)
    elif kind == 'wink':
        draw.line([(18, 29), (20, 30), (22, 29)], fill=0)
        im.paste(BASE.crop((31, 27, 36, 32)), (31, 27))


def mouth(im, kind='smile'):
    draw = ImageDraw.Draw(im)
    draw.rectangle((25, 33, 29, 36), fill=1)
    if kind == 'smile':
        draw.line([(25, 34), (26, 35), (28, 35), (29, 34)], fill=0)
    elif kind == 'open':
        draw.ellipse((25, 33, 29, 36), fill=0)
        draw.line((26, 33, 28, 33), fill=1)
    elif kind == 'small':
        draw.ellipse((26, 34, 28, 36), outline=0, fill=1)
    elif kind == 'worried':
        draw.line([(25, 36), (26, 35), (28, 35), (29, 36)], fill=0)
    else:
        draw.line((26, 35, 28, 35), fill=0)


def heart(draw, x, y):
    for row, line in enumerate(('.##.##.', '#######', '#######', '.#####.', '..###..', '...#...')):
        for column, bit in enumerate(line):
            if bit == '#':
                draw.point((x + column, y + row), fill=0)


def sparkle(draw, x, y):
    draw.line((x - 2, y, x + 2, y), fill=0)
    draw.line((x, y - 2, x, y + 2), fill=0)


def tail(im, amount):
    if not amount:
        return im
    region = Image.new('1', (64, 64), 0)
    ImageDraw.Draw(region).polygon([(43, 58), (49, 55), (54, 50), (56, 44), (61, 40),
                                   (62, 40), (62, 45), (58, 50), (55, 55), (50, 58), (43, 60)], fill=1)
    ink = ImageChops.logical_and(ImageChops.invert(im), region)
    im.paste(1, (0, 0), region)
    im.paste(0, (0, 0), ink.rotate(amount, Image.Resampling.NEAREST, center=(44, 57), fillcolor=0))
    return im


def rice_bowl(im, bite):
    draw = ImageDraw.Draw(im)
    # White rice has a scalloped silhouette and sparse grains; bowl stays white.
    draw.polygon([(18, 42), (18, 40), (20, 39), (21, 37), (24, 37), (25, 36),
                  (28, 37), (30, 36), (33, 37), (35, 39), (37, 39), (38, 42)], fill=1, outline=0)
    draw.line([(18, 42), (20, 48), (24, 51), (33, 51), (36, 48), (38, 42)], fill=1, width=3)
    draw.line([(18, 42), (20, 48), (24, 51), (33, 51), (36, 48), (38, 42), (18, 42)], fill=0)
    draw.line((25, 52, 32, 52), fill=0)
    for x, y in ((23, 40), (27, 39), (32, 40)):
        draw.point((x, y), fill=0)
    # Whale mark on the bowl references the supplied illustration.
    draw.ellipse((25, 45, 31, 48), fill=0)
    draw.line([(31, 47), (33, 45), (33, 47)], fill=0)
    if bite:
        draw.line([(12, 38), (21, 35), (24, 35)], fill=0)
        draw.point((24, 34), fill=0)


def frame(name, i, count, loop):
    im = BASE.copy()
    phase = i / count
    # Brief preparation/settle frames preserve continuity for one-shot reactions.
    expressive = loop or 3 <= i < count - 4
    angle = round(3 * math.sin(phase * math.tau))
    if name in ('excited', 'play', 'pat'):
        angle = round(6 * math.sin(phase * math.tau * 2))
    if name == 'sleeping':
        angle = 0
    im = tail(im, angle)
    draw = ImageDraw.Draw(im)
    if expressive:
        if name in ('content', 'pat', 'comfort'):
            eyes(im, 'happy' if name != 'comfort' else 'closed')
            mouth(im)
            if 7 <= i < count - 7:
                heart(draw, 52, 10 - min(3, i // 10))
        elif name == 'curious':
            eyes(im, 'normal', 1 if i > count // 3 else 0)
            mouth(im, 'small')
            if count // 3 < i < count * 3 // 4:
                draw.line([(53, 10), (55, 8), (58, 9), (58, 11), (55, 13), (55, 14)], fill=0)
                draw.point((55, 16), fill=0)
        elif name == 'excited':
            eyes(im, 'wide' if i < 12 else 'happy')
            mouth(im, 'open' if i % 12 < 6 else 'smile')
            if i % 16 < 10:
                sparkle(draw, 7, 15)
                sparkle(draw, 55, 9)
        elif name == 'shy':
            eyes(im, 'half')
            mouth(im)
            draw.line((17, 33, 20, 34), fill=0)
            draw.line((33, 33, 36, 34), fill=0)
            if i > 9:
                heart(draw, 52, 9)
        elif name == 'surprised':
            eyes(im, 'wide')
            mouth(im, 'open')
            draw.line((55, 8, 55, 13), fill=0, width=2)
            draw.point((55, 16), fill=0)
        elif name == 'proud':
            eyes(im, 'wink' if i < 12 else 'half')
            mouth(im)
            sparkle(draw, 54, 13)
        elif name == 'worried':
            eyes(im, 'worried')
            mouth(im, 'worried')
            if 10 < i < 23:
                draw.line([(51, 15), (53, 18), (51, 19), (50, 18), (51, 15)], fill=0)
        elif name in ('sleepy', 'sleeping'):
            eyes(im, 'closed' if name == 'sleeping' or 17 < i < 32 else 'half')
            mouth(im, 'open' if name == 'sleepy' and 18 < i < 28 else 'closed')
            if name == 'sleeping':
                y = 12 - (i // 16) % 3
                draw.line([(5, y), (8, y), (5, y + 3), (8, y + 3)], fill=0)
        elif name == 'rice':
            if i < 12:
                eyes(im, 'wide')
                mouth(im, 'open')
                sparkle(draw, 54, 10)
            else:
                eyes(im, 'happy' if i > 31 else 'closed')
                mouth(im, 'small' if i % 6 < 3 else 'smile')
                rice_bowl(im, 14 < i < 31 and i % 8 < 4)
                if i > 32:
                    heart(draw, 52, 8)
        elif name in ('wave', 'full'):
            eyes(im, 'happy' if name == 'wave' else 'half')
            mouth(im)
            x = [-1, 0, 1, 0][i // 3 % 4]
            draw.polygon([(22, 39), (22, 43), (19, 45), (15, 42), (12, 37), (16, 35), (18, 39)], fill=0)
            draw.line([(14, 38), (16, 37), (18, 41)], fill=1)
            draw.ellipse((11 + x, 31, 16 + x, 36), fill=1, outline=0)
            draw.line((13 + x, 32, 13 + x, 34), fill=0)
            if name == 'full':
                draw.line((52, 12, 58, 12), fill=0)
        elif name == 'play':
            eyes(im, 'normal', -1 if i % 20 < 10 else 0)
            mouth(im)
            y = 40 + [0, 2, 5, 8, 5, 2][i // 2 % 6]
            draw.ellipse((2, y, 9, y + 7), fill=1, outline=0)
            draw.line((5, y + 1, 5, y + 6), fill=0)
        elif name == 'listening':
            eyes(im, 'normal', 1 if 8 < i < 20 else 0)
            mouth(im, 'closed')
        elif name == 'thinking':
            eyes(im, 'half' if i > 27 else 'normal', -1 if 5 < i < 24 else 0)
            mouth(im, 'closed')
            for x in range(5, 12, 3):
                if i % 18 > x:
                    draw.point((x, 16), fill=0)
        elif name == 'speaking':
            mouth(im, ('closed', 'small', 'open', 'small', 'smile')[i % 5])
    if name in ('idle', 'listening', 'speaking', 'content', 'curious'):
        blink = {14: 'half', 15: 'closed', 16: 'half', 63: 'half', 64: 'closed', 65: 'half', 68: 'closed'}
        if i in blink:
            eyes(im, blink[i])
    # Slow breathing with a long pause, independent of the blink schedule.
    dy = -1 if name in ('idle', 'sleeping', 'sleepy') and 22 <= i % 64 <= 39 else 0
    if dy:
        shifted = Image.new('1', (64, 64), 1)
        shifted.paste(im, (0, dy))
        im = shifted
    return im


def pack(im):
    return bytes(sum((im.getpixel((x + bit, y)) == 0) << bit for bit in range(8))
                 for y in range(64) for x in range(0, 64, 8))


def lcd(im, scale=3):
    color = Image.new('RGB', im.size)
    color.putdata([(239, 182, 72) if pixel else (41, 31, 23) for pixel in im.getdata()])
    return color.resize((im.width * scale, im.height * scale), Image.Resampling.NEAREST)


# Small UI bitmaps for the pet app, '#' = ink. Packed as XBM (LSB-first rows).
ICONS = {
    # 9x9 menu icons
    'pat': ['...#.#...', '..#.#.#.#', '..#.#.#.#', '.##.....#', '#.#.....#',
            '#.......#', '.#......#', '..#....#.', '...####..'],
    'feed': ['..#.#.#..', '.#.....#.', '#.......#', '#########', '#.......#',
             '.#.....#.', '..#####..', '...#.#...', '...###...'],
    'play': ['..#####..', '.#.....#.', '#..###..#', '#.#####.#', '#.......#',
             '#.......#', '#.......#', '.#.....#.', '..#####..'],
    'catch': ['.#.....#.', '.........', '....#....', '#........', '.........',
              '#########', '#.......#', '.#.....#.', '..#####..'],
    'sleep': ['...####..', '..###....', '.###.....', '.###.....', '.###.....',
              '.####....', '..####..#', '...######', '....####.'],
    'stats': ['.........', '#.......#', '#..#....#', '#..#.#..#', '#..#.#.##',
              '#.##.#.##', '#.##.####', '#########', '.........'],
    'wake': ['....#....', '.#.....#.', '...###...', '..#...#..', '#.#...#.#',
             '..#...#..', '...###...', '.#.....#.', '....#....'],
    # 5x5 stat icons and a flash "+"
    'mini_food': ['.#.#.', '#####', '#...#', '.###.', '..#..'],
    'mini_energy': ['..##.', '.##..', '####.', '..##.', '.##..'],
    'mini_joy': ['##.##', '#####', '#####', '.###.', '..#..'],
    'plus': ['..#..', '..#..', '#####', '..#..', '..#..'],
    # 3x5 carousel arrows
    'left': ['..#', '.##', '###', '.##', '..#'],
    'right': ['#..', '##.', '###', '##.', '#..'],
    # Catch Rice sprites
    'bowl': ['#...............#', '#################', '#...............#', '.#.....###.....#.',
             '..#...........#..', '...###########...'],
    'grain': ['.#.', '###', '###', '.#.'],
    'onigiri': ['...#...', '..#.#..', '.#...#.', '#.....#', '#.###.#', '##...##', '#######'],
}


def xbm(rows):
    width = max(len(r) for r in rows)
    out = []
    for row in rows:
        row = row.ljust(width, '.')
        for x in range(0, width, 8):
            out.append(sum((row[x + b] == '#') << b for b in range(min(8, width - x))))
    return width, len(rows), bytes(out)


def c_bytes(data, indent='    '):
    return [indent + ','.join(f'0x{x:02x}' for x in data[i:i + 16]) + ',' for i in range(0, len(data), 16)]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    APP.mkdir(parents=True, exist_ok=True)
    unique, lookup, clips, sprites = [], {}, [], {}
    for name, label, count, loop in CLIPS:
        sequence, frames = [], []
        folder = OUT / 'frames' / name
        folder.mkdir(parents=True, exist_ok=True)
        for i in range(count):
            sprite = frame(name, i, count, loop)
            data = pack(sprite)
            if data not in lookup:
                lookup[data] = len(unique)
                unique.append(data)
            sequence.append(lookup[data])
            frames.append(sprite)
            sprite.save(folder / f'{i:02d}.png')
        clips.append(dict(id=name, label=label, frames=sequence, tick_ms=TICK, loop=loop))
        sprites[name] = frames
        colors = [lcd(sprite, 4) for sprite in frames]
        colors[0].save(OUT / f'{name}.gif', save_all=True, append_images=colors[1:],
                       duration=TICK, loop=0, optimize=False, disposal=2)
    raw = b''.join(unique)
    # Opacity masks keep the white face/clothes and a 1 px paper halo so the
    # character stays readable over the patterned rooms.
    masks = b''.join(pack_mask(opacity_mask(unpack_frame(data))) for data in unique)
    for data, mask in zip(unique, (masks[i:i + 512] for i in range(0, len(masks), 512))):
        assert all(ink & ~opaque == 0 for ink, opaque in zip(data, mask))
    (OUT / 'pet_frames.bin').write_bytes(raw)
    (OUT / 'pet_masks.bin').write_bytes(masks)
    manifest = dict(version=2, width=64, height=64, bytes_per_frame=512,
                    packing='row-major, LSB-first, black=1', unique_frame_count=len(unique),
                    masks='pet_masks.bin (same order, 1 = opaque)',
                    source='../whale-girl/source/accepted-screen.png', favorite_food='white_rice',
                    food_reference='source/favorite-rice.png', animations=clips)
    (OUT / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    # Small header for the controller: clip ids, lengths and looping only.
    clip_header = ['// Generated by scripts/make_pet_assets.py.', '#pragma once',
                   '#include <stdint.h>', '#include <stdbool.h>',
                   f'#define PET_CLIP_COUNT {len(clips)}', 'typedef enum {']
    clip_header += [f'    PetClip{name.title()},' for name, *_ in CLIPS]
    clip_header += ['} PetClip;',
                    'static const uint16_t pet_clip_length[PET_CLIP_COUNT] = {' +
                    ','.join(str(len(c['frames'])) for c in clips) + '};',
                    'static const bool pet_clip_loop[PET_CLIP_COUNT] = {' +
                    ','.join(str(c['loop']).lower() for c in clips) + '};']
    (APP / 'pet_clips.h').write_text('\n'.join(clip_header) + '\n', encoding='ascii')
    header = ['// Generated by scripts/make_pet_assets.py. Native 1-bit pixels.',
              '#pragma once', '#include "pet_clips.h"',
              f'#define PET_FRAME_COUNT {len(unique)}',
              'static const uint8_t pet_fallback[512] = {']
    header += c_bytes(pack(BASE)) + ['};', 'static const uint8_t pet_fallback_mask[512] = {']
    header += c_bytes(pack_mask(opacity_mask(BASE))) + ['};']
    for night in (False, True):
        room = app_room(night)
        header.append(f'static const uint8_t pet_room_{"night" if night else "day"}[1024] = {{')
        header += c_bytes(pack_wide(room)) + ['};']
    for name, rows in ICONS.items():
        w, h, data = xbm(rows)
        header.append(f'#define pet_icon_{name}_w {w}')
        header.append(f'#define pet_icon_{name}_h {h}')
        header.append(f'static const uint8_t pet_icon_{name}[] = {{' + ','.join(f'0x{x:02x}' for x in data) + '};')
    for clip in clips:
        header.append(f'static const uint16_t pet_seq_{clip["id"]}[] = {{' + ','.join(map(str, clip['frames'])) + '};')
    header += ['typedef struct { const char* name; const uint16_t* frames; uint16_t count; bool loop; } PetAnimation;',
               'static const PetAnimation pet_animations[PET_CLIP_COUNT] = {']
    header += [f'    {{"{c["id"]}", pet_seq_{c["id"]}, {len(c["frames"])}, {str(c["loop"]).lower()}}},' for c in clips]
    header += ['};']
    (APP / 'pet_art.h').write_text('\n'.join(header) + '\n', encoding='ascii')
    resources = ROOT / 'firmware/flipper/applications/services/deepflipper/resources/deepflipper'
    (resources / 'pet_frames.bin').write_bytes(raw)
    (resources / 'pet_masks.bin').write_bytes(masks)
    font_path = Path('C:/Windows/Fonts/msyh.ttc')
    for candidate in (font_path, Path('/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')):
        if candidate.exists():
            font = ImageFont.truetype(str(candidate), 19)
            break
    else:
        font = ImageFont.load_default()
    sheet = Image.new('RGB', (1224, 1144), '#182630')
    draw = ImageDraw.Draw(sheet)
    draw.text((20, 14), '鲸鱼娘 · 陪伴表情与白米饭', font=font, fill='#f6e8d1')
    for index, (name, label, count, _) in enumerate(CLIPS):
        x, y = 20 + index % 6 * 200, 58 + index // 6 * 220
        draw.text((x, y), label, font=font, fill='#f6e8d1')
        sheet.paste(lcd(sprites[name][min(count - 6, count * 2 // 3)], 2), (x, y + 30))
        sheet.paste(sprites[name][count // 2].convert('RGB'), (x + 132, y + 94))
    draw.text((220, 732), '右侧小图为原生 64×64 像素。', font=font, fill='#c7d5d5')
    draw.text((20, 940), '开饭啦 → 捧碗吃饭 → 满足', font=font, fill='#c7d5d5')
    for index, i in enumerate((7, 18, 26, 38, 44)):
        sheet.paste(lcd(sprites['rice'][i], 2), (20 + index * 160, 980))
    sheet.save(OUT / 'expressions.png')
    print(f'Pet art: {len(clips)} expressions, {len(unique)} unique frames, {len(raw)} SD bytes '
          f'+ {len(masks)} mask bytes. Favorite food: white rice.')


def unpack_frame(data):
    im = Image.new('1', (64, 64), 1)
    for y in range(64):
        for x in range(64):
            if data[y * 8 + x // 8] >> (x % 8) & 1:
                im.putpixel((x, y), 0)
    return im


if __name__ == '__main__':
    main()
