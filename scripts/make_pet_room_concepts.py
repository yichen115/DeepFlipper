"""Native-pixel room art shared by the home screen, the pet app and previews.

Detail comes from sparse, regular textures (wallpaper motifs, wainscot,
floorboards) plus a few well-outlined props. No dithering on large areas,
so every object keeps a clean silhouette at 128x64. Runtime values (time,
battery, bubbles) are drawn by firmware into the blank screens.
"""
from collections import deque
from pathlib import Path
import base64
import json
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/whale-pet/concepts'


def pack(im):
    return bytes(sum((im.getpixel((x + b, y)) == 0) << b for b in range(8))
                 for y in range(im.height) for x in range(0, im.width, 8))


def pack_mask(im):
    # Pillow mode-1 masks may contain 0/1 rather than 0/255. Inverting those
    # produces 255/254, so the art packer's "black == 0" test erased all masks.
    return bytes(sum(bool(im.getpixel((x + b, y))) << b for b in range(8))
                 for y in range(im.height) for x in range(0, im.width, 8))


def put(im, x, y, v):
    if 0 <= x < im.width and 0 <= y < im.height:
        im.putpixel((x, y), v)


def stamp(im, x0, y0, rows):
    for dy, row in enumerate(rows):
        for dx, ch in enumerate(row):
            x, y = x0 + dx, y0 + dy
            if not (0 <= x < im.width and 0 <= y < im.height):
                continue
            if ch == '#':
                im.putpixel((x, y), 0)
            elif ch == '.':
                im.putpixel((x, y), 1)


# --------------------------------------------------------------- shared parts
def wallpaper(im, kind, top=0, bottom=36):
    for y in range(top, bottom + 1):
        for x in range(128):
            if kind == 'diamond':
                # Tiny 4-dot diamonds on an offset 10x8 lattice.
                ox = 5 if (y // 8) % 2 else 0
                cx, cy = (x - ox) % 10, y % 8
                if (cx, cy) in ((4, 3), (3, 4), (5, 4), (4, 5)):
                    im.putpixel((x, y), 0)
            elif kind == 'dots':
                # Small rice-grain dashes on a sparse offset grid.
                ox = 4 if (y // 7) % 2 else 0
                cx, cy = (x - ox) % 8, y % 7
                if cy == 3 and cx in (3, 4):
                    im.putpixel((x, y), 0)


def wainscot(im, top=37, floor=50):
    d = ImageDraw.Draw(im)
    d.line((0, top, 127, top), fill=0)
    d.line((0, top + 2, 127, top + 2), fill=0)
    d.line((0, floor, 127, floor), fill=0)
    d.line((0, floor + 2, 127, floor + 2), fill=0)


def floorboards(im, top=53):
    d = ImageDraw.Draw(im)
    for row, y in enumerate((56, 60)):
        d.line((0, y, 127, y), fill=0)
    for row, (y0, y1) in enumerate(((top, 55), (57, 59), (61, 63))):
        for x in range(row * 9 + 4, 128, 27):
            d.line((x, y0, x, y1), fill=0)


def rug(im, x0=37, x1=92, y0=55, y1=63):
    d = ImageDraw.Draw(im)
    d.ellipse((x0, y0, x1, y1 + 4), fill=1, outline=0)
    d.ellipse((x0 + 4, y0 + 2, x1 - 4, y1 + 2), outline=0)
    for x in range(x0 + 6, x1 - 5, 3):
        im.putpixel((x, y0 + 4 + (1 if abs(x - (x0 + x1) // 2) > 20 else 0)), 0)


def window(im, x0, y0, x1, y1, night):
    """Two-pane window with a scalloped valance and tied side curtains."""
    d = ImageDraw.Draw(im)
    d.rectangle((x0, y0, x1, y1), fill=0 if night else 1, outline=0)
    mx = (x0 + x1) // 2
    if night:
        cx = x1 - 6
        d.ellipse((cx - 3, y0 + 4, cx + 3, y0 + 10), fill=1)
        d.ellipse((cx - 1, y0 + 3, cx + 4, y0 + 8), fill=0)
        for dx, dy in ((3, 5), (6, 11), (9, 7), (x1 - x0 - 4, 14), (4, 14)):
            put(im, x0 + dx, y0 + dy, 1)
        d.line((mx, y0, mx, y1), fill=1)
        d.line((mx - 1, y0, mx - 1, y1), fill=0)
        d.line((mx + 1, y0, mx + 1, y1), fill=0)
    else:
        sx, sy = x1 - 6, y0 + 7
        d.ellipse((sx - 2, sy - 2, sx + 2, sy + 2), outline=0)
        for ddx, ddy in ((-4, 0), (4, 0), (0, -4), (0, 4)):
            put(im, sx + ddx, sy + ddy, 0)
        d.line([(x0 + 1, y1 - 2), (x0 + 6, y1 - 5), (mx, y1 - 3), (x1 - 5, y1 - 6), (x1 - 1, y1 - 3)], fill=0)
        d.line((mx, y0, mx, y1), fill=0)
    # Valance with scallops.
    d.rectangle((x0 - 2, y0, x1 + 2, y0 + 2), fill=1, outline=0)
    for x in range(x0 - 1, x1 + 2, 3):
        put(im, x, y0 + 3, 0)
        put(im, x + 1, y0 + 3, 0)
    # Side curtains, tied at two-thirds height.
    for side in (0, 1):
        cx0 = x0 - 2 if side == 0 else x1 - 1
        tie = y0 + (y1 - y0) * 2 // 3
        d.rectangle((cx0, y0 + 3, cx0 + 3, y1), fill=1, outline=0)
        d.line((cx0 + 1 + side, y0 + 4, cx0 + 1 + side, tie - 1), fill=0)
        d.line((cx0, tie, cx0 + 3, tie), fill=0)
    # Sill.
    d.rectangle((x0 - 3, y1 + 1, x1 + 3, y1 + 2), fill=1, outline=0)


def bubble(im, x, y, w, h):
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((x, y, x + w - 1, y + h - 1), radius=3, fill=1, outline=0)
    stamp(im, x - 3, y + h - 4, ['...#', '.##.', '#...'])


# --------------------------------------------------------------- home room
def room(night=False, dinner=False):
    """Home screen room. Clock/cooker screens stay blank: firmware draws them."""
    del dinner  # kept for older concept callers
    return home_room(night)


def home_room(night=False, steam=0, phase=0):
    im = Image.new('1', (128, 64), 1)
    d = ImageDraw.Draw(im)
    wallpaper(im, 'diamond', 0, 35)
    wainscot(im)
    floorboards(im)
    rug(im)
    # Window with curtains, top-left.
    d.rectangle((0, 0, 31, 18), fill=1)
    window(im, 4, 1, 26, 15, night)
    # Alarm clock (time is drawn on its screen) on a two-drawer cabinet.
    d.rectangle((0, 18, 32, 38), fill=1)
    d.ellipse((2, 18, 7, 23), fill=1, outline=0)
    d.ellipse((25, 18, 30, 23), fill=1, outline=0)
    d.line((5, 18, 27, 18), fill=0)
    d.line((16, 17, 16, 18), fill=0)
    d.rounded_rectangle((0, 20, 32, 37), radius=3, fill=1, outline=0)
    d.rectangle((3, 23, 29, 35), outline=0)
    d.line((4, 38, 6, 38), fill=0)
    d.line((26, 38, 28, 38), fill=0)
    d.rectangle((0, 39, 30, 52), fill=1, outline=0)
    d.line((0, 41, 30, 41), fill=0)
    d.line((15, 41, 15, 52), fill=0)
    for kx in (7, 22):
        d.rectangle((kx - 1, 45, kx + 1, 47), outline=0)
    d.line((2, 53, 2, 54), fill=0)
    d.line((28, 53, 28, 54), fill=0)
    # Shelf with a whale plush and two books, top-right.
    d.rectangle((99, 0, 127, 13), fill=1)
    d.line((99, 10, 127, 10), fill=0)
    d.line((101, 11, 101, 12), fill=0)
    d.line((125, 11, 125, 12), fill=0)
    d.rectangle((101, 2, 103, 9), outline=0)
    d.rectangle((104, 4, 106, 9), outline=0)
    d.line((102, 4, 102, 7), fill=0)
    stamp(im, 112, 3, ['...####....',
                       '..#....#.#.',
                       '.#.#....##.',
                       '.#......#..',
                       '..######...',
                       '...........'])
    # Counter with cupboard doors and a wide rice cooker (battery on its LCD).
    d.rectangle((97, 14, 127, 52), fill=1)
    d.rectangle((97, 44, 127, 52), fill=1, outline=0)
    d.line((97, 46, 127, 46), fill=0)
    d.line((112, 46, 112, 52), fill=0)
    d.point((110, 49), fill=0)
    d.point((114, 49), fill=0)
    d.rounded_rectangle((98, 24, 127, 43), radius=4, fill=1, outline=0)
    d.chord((100, 17, 125, 31), 180, 360, fill=1, outline=0)
    d.line((100, 24, 125, 24), fill=0)
    d.rectangle((110, 16, 115, 18), fill=0)
    d.line((96, 31, 97, 31), fill=0)          # side handle
    d.rectangle((100, 27, 125, 38), outline=0)
    d.line((98, 40, 127, 40), fill=0)
    for bx in (104, 108):
        d.point((bx, 42), fill=0)
    # Door mat towards the pet room (short Right); the lock is drawn over it.
    d.rounded_rectangle((106, 55, 127, 63), radius=2, fill=1, outline=0)
    d.rounded_rectangle((108, 57, 125, 61), radius=1, outline=0)
    stamp(im, 112, 57, ['.#...#...', '..#...#..', '...#...#.', '..#...#..', '.#...#...'])
    for i in range(steam):
        x = 108 + i * 8
        p = (phase + i * 2) % 4
        wisp = [(x, 15), (x - 1, 14), (x - 1, 13), (x, 12), (x, 11), (x - 1, 10), (x - 1, 9)]
        for k, (wx, wy) in enumerate(wisp):
            if k >= p and k < p + 4:
                put(im, wx + (1 if p % 2 else 0), wy, 0)
    return im


# --------------------------------------------------------------- app room
def app_room(night=False):
    im = Image.new('1', (128, 64), 1)
    d = ImageDraw.Draw(im)
    wallpaper(im, 'dots', 0, 35)
    wainscot(im)
    floorboards(im)
    rug(im)
    # Plain stats card (values drawn by the app), top-left. No pin: at 1x a
    # pin read as a stray "+" above the bars.
    d.rectangle((0, 0, 32, 25), fill=1)
    d.rectangle((0, 2, 31, 23), outline=0)
    d.line((1, 24, 31, 24), fill=0)
    d.line((32, 3, 32, 24), fill=0)
    # Toy basket with ball, bottom-left.
    d.rectangle((0, 33, 30, 52), fill=1)
    d.ellipse((5, 31, 15, 41), fill=1, outline=0)
    d.arc((7, 33, 13, 39), 200, 340, fill=0)
    d.line((10, 31, 10, 41), fill=0)
    d.ellipse((15, 34, 22, 41), fill=1, outline=0)
    d.point((18, 36), fill=0)
    d.polygon([(2, 40), (28, 40), (25, 52), (5, 52)], fill=1, outline=0)
    for x in range(6, 26, 4):
        d.line((x, 41, x + 1, 51), fill=0)
    d.line((3, 44, 27, 44), fill=0)
    # Window, top-right.
    d.rectangle((98, 0, 127, 22), fill=1)
    window(im, 103, 1, 123, 17, night)
    # Dining table with tablecloth, rice bowl and chopsticks.
    d.rectangle((97, 26, 127, 52), fill=1)
    d.rectangle((98, 38, 127, 41), fill=1, outline=0)
    for x in range(99, 127, 4):
        d.chord((x, 39, x + 4, 44), 0, 180, fill=1, outline=0)
    d.line((101, 44, 101, 53), fill=0)
    d.line((124, 44, 124, 53), fill=0)
    d.line((101, 48, 124, 48), fill=0)
    stamp(im, 104, 29, ['--#-#-#--',
                        '-#.....#-',
                        '#.......#',
                        '#########',
                        '#.......#',
                        '-#.....#-',
                        '--#####--',
                        '---###---'])
    d.line((116, 37, 125, 33), fill=0)
    d.line((117, 37, 126, 34), fill=0)
    return im


# 4x7 digits for the clock and rice-cooker screens (1px gaps, 1px colon).
DIGITS = {
    '0': ['.##.', '#..#', '#..#', '#..#', '#..#', '#..#', '.##.'],
    '1': ['.#..', '##..', '.#..', '.#..', '.#..', '.#..', '###.'],
    '2': ['.##.', '#..#', '...#', '..#.', '.#..', '#...', '####'],
    '3': ['###.', '...#', '...#', '.##.', '...#', '...#', '###.'],
    '4': ['..#.', '.##.', '#.#.', '#.#.', '####', '..#.', '..#.'],
    '5': ['####', '#...', '###.', '...#', '...#', '#..#', '.##.'],
    '6': ['.##.', '#...', '#...', '###.', '#..#', '#..#', '.##.'],
    '7': ['####', '...#', '..#.', '..#.', '.#..', '.#..', '.#..'],
    '8': ['.##.', '#..#', '#..#', '.##.', '#..#', '#..#', '.##.'],
    '9': ['.##.', '#..#', '#..#', '.###', '...#', '...#', '.##.'],
    ':': ['.', '.', '#', '.', '#', '.', '.'],
    '%': ['##.#', '##.#', '..#.', '.#..', '.#..', '#.##', '#.##'],
}


def digits_width(text):
    return sum(len(DIGITS[c][0]) for c in text) + len(text) - 1


def digits(im, cx, top, text):
    """Draw centred on column cx (box inner centre), glyph top at `top`."""
    x = cx - digits_width(text) // 2
    for c in text:
        stamp(im, x, top, DIGITS[c])
        x += len(DIGITS[c][0]) + 1
    return x


def opacity_mask(im):
    # Flood only border-connected paper. Preserve white face/clothing interiors,
    # and a one-pixel paper halo around the dark silhouette for clear separation.
    outside = set()
    q = deque()
    for x in range(64):
        for y in (0, 63):
            if im.getpixel((x, y)) != 0:
                outside.add((x, y)); q.append((x, y))
    for y in range(64):
        for x in (0, 63):
            if im.getpixel((x, y)) != 0:
                outside.add((x, y)); q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < 64 and 0 <= ny < 64 and (nx, ny) not in outside and im.getpixel((nx, ny)) != 0:
                outside.add((nx, ny)); q.append((nx, ny))
    mask = Image.new('1', (64, 64), 1)
    for point in outside:
        mask.putpixel(point, 0)
    for y in range(64):
        for x in range(64):
            if im.getpixel((x, y)) == 0:
                for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if 0 <= nx < 64 and 0 <= ny < 64:
                        mask.putpixel((nx, ny), 1)
    return mask


def unpack(data, width=64, height=64):
    im = Image.new('1', (width, height), 1)
    for y in range(height):
        for x in range(width):
            im.putpixel((x, y), 0 if data[y * (width // 8) + x // 8] >> (x % 8) & 1 else 1)
    return im


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / 'assets/whale-pet/manifest.json').read_text(encoding='utf-8'))
    raw = (ROOT / 'assets/whale-pet/pet_frames.bin').read_bytes()
    backgrounds = {'idle': room(), 'rice': room(dinner=True), 'sleeping': room(night=True)}
    masks = bytearray()
    for i in range(manifest['unique_frame_count']):
        mask = opacity_mask(unpack(raw[i * 512:(i + 1) * 512]))
        encoded = pack_mask(mask)
        assert any(encoded), f'Empty opacity mask for pet frame {i}'
        sprite = raw[i * 512:(i + 1) * 512]
        assert all(ink & ~opaque == 0 for ink, opaque in zip(sprite, encoded)), i
        masks.extend(encoded)
    frames = {}
    for name in ('idle', 'rice', 'sleeping'):
        background = backgrounds[name]
        background.save(OUT / f'room-{name}-native.png')
        clip = next(c for c in manifest['animations'] if c['id'] == name)
        sequence = []
        for idx in clip['frames']:
            sprite = unpack(raw[idx * 512:(idx + 1) * 512])
            view = background.copy()
            view.paste(sprite, (33, 0), opacity_mask(sprite))
            sequence.append(view)
        frames[name] = sequence
        sequence[len(sequence) * 3 // 5].save(OUT / f'room-{name}-view.png')
    payload = {'rooms': {name: base64.b64encode(pack(im)).decode('ascii') for name, im in backgrounds.items()},
               'masks': base64.b64encode(masks).decode('ascii')}
    (ROOT / 'build/pet-room-data.json').write_text(json.dumps(payload, separators=(',', ':')), encoding='utf-8')
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 20)
    sheet = Image.new('RGB', (1048, 670), '#1b292d')
    d = ImageDraw.Draw(sheet)
    for n, (name, label) in enumerate((('idle', '日常 · 窗边小窝'), ('rice', '开饭 · 白米饭时间'), ('sleeping', '晚安 · 月亮和小灯'))):
        im = frames[name][len(frames[name]) * 3 // 5].convert('RGB')
        im.putdata([(239, 182, 72) if p[0] else (41, 31, 23) for p in im.getdata()])
        x, y = 20 + (n % 2) * 516, 46 + (n // 2) * 316
        d.text((x, y - 30), label, font=font, fill='#f4ead9')
        sheet.paste(im.resize((512, 256), Image.Resampling.NEAREST), (x, y))
    d.text((538, 396), '全部画面均为 128×64 单色像素', font=font, fill='#f4ead9')
    d.text((538, 434), '角色、房间、操作浮层分开绘制', font=font, fill='#c5d1cb')
    d.text((538, 472), '方案预览，尚未写入固件', font=font, fill='#c5d1cb')
    sheet.save(OUT / 'room-scenes-v2.png')
    print('Room concepts and', manifest['unique_frame_count'], 'opaque sprite masks generated.')


if __name__ == '__main__':
    main()
