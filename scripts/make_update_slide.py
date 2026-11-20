"""Render the single post-update slide (assets/slideshow/update_default/frame_00.png).

The upstream Unleashed slideshow had four pages (Unleashed logo, scam warning,
Discord and GitHub links). DeepFlipper only shows one "firmware updated" page:
the original 64x64 whale girl on the left, text on the right and an OK button.
A single-frame slideshow is closed by OK / Right / Back (desktop_view_slideshow.c).

Usage: python scripts/make_update_slide.py [--preview out.png]
"""
from pathlib import Path
import argparse
import sys

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pixel_font import PixelFont  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
FONT_SOURCE = ROOT / 'firmware/flipper/lib/u8g2/u8g2_fonts.c'
SPRITE = ROOT / 'assets/whale-girl/frames/happy/00.png'
SLIDE_DIR = ROOT / 'firmware/flipper/assets/slideshow/update_default'
W, H = 128, 64


def draw_button(img, font, right, bottom, label):
    """Rounded button with a heavier left/bottom edge, matching the stock slides."""
    arrow_w = 4
    inner = font.width(label) + 3 + arrow_w
    w, h = inner + 10, 15
    x0, y0 = right - w, bottom - h
    px = lambda x, y: img.putpixel((x, y), 0)  # noqa: E731
    for x in range(x0 + 3, x0 + w - 2):
        px(x, y0)                       # top edge
        px(x, y0 + h - 1)               # bottom edge
        px(x, y0 + h)                   # bottom shadow
    for y in range(y0 + 3, y0 + h - 2):
        px(x0, y)                       # left edge (2 px thick)
        px(x0 + 1, y)
        px(x0 + w - 1, y)               # right edge
    for x, y in ((x0 + 1, y0 + 2), (x0 + 2, y0 + 1), (x0 + w - 2, y0 + 1), (x0 + w - 1, y0 + 2),
                 (x0 + 1, y0 + h - 2), (x0 + 2, y0 + h - 1), (x0 + w - 2, y0 + h - 2),
                 (x0 + 2, y0 + h - 2), (x0 + 1, y0 + h - 3)):
        px(x, y)
    text_x = x0 + 5
    baseline = y0 + 11
    font.draw(img, text_x, baseline, label)
    ax = text_x + font.width(label) + 3
    for i in range(arrow_w):            # solid right-pointing triangle
        for y in range(baseline - 7 + i, baseline - i):
            px(ax + i, y)


def render():
    primary = PixelFont(FONT_SOURCE, 'u8g2_font_helvB08_tr')
    secondary = PixelFont(FONT_SOURCE, 'u8g2_font_haxrcorp4089_tr')
    img = Image.new('1', (W, H), 1)
    sprite = Image.open(SPRITE).convert('1')
    assert sprite.size == (64, 64), sprite.size
    img.paste(sprite, (0, 0))

    right = W - 2
    primary.draw(img, right, 12, 'DeepFlipper', align='right')
    secondary.draw(img, right, 25, 'Firmware', align='right')
    secondary.draw(img, right, 35, 'updated!', align='right')
    draw_button(img, secondary, right, 61, 'OK')
    return img


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--preview', help='also write an RGB 4x preview here')
    args = parser.parse_args()
    img = render()
    SLIDE_DIR.mkdir(parents=True, exist_ok=True)
    for stale in SLIDE_DIR.glob('frame_*.png'):
        if stale.name != 'frame_00.png':
            stale.unlink()   # slideshow.py packs frame_00, frame_01, ... until a gap
    img.convert('RGB').save(SLIDE_DIR / 'frame_00.png')
    if args.preview:
        big = img.convert('RGB').resize((W * 4, H * 4), Image.NEAREST)
        canvas = Image.new('RGB', (W * 4 + 16, H * 5 + 24), (27, 38, 48))
        canvas.paste(img.convert('RGB'), (8, 8))
        canvas.paste(big, (8, H + 16))
        canvas.save(args.preview)
    print(f'wrote {SLIDE_DIR / "frame_00.png"} (1 frame)')


if __name__ == '__main__':
    main()
