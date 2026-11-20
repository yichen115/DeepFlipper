"""Turn the production pet drawing captures (tests/pet_render_test.c) into PNGs.

Run scripts/test_native.py first; it rasterizes the real whale_pet.c drawing
code with the firmware fonts into build/host/pet-*.raw (128x64, 1 byte/pixel).
"""
from pathlib import Path
import sys
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/whale-pet/app'
SCENES = ('welcome', 'menu-feed', 'rice', 'item-Catch', 'night', 'stats', 'faces',
          'game', 'game-over', 'damaged')
PAPER, INK = (239, 182, 72), (41, 31, 23)


def load(source, name):
    raw = (Path(source) / f'pet-{name}.raw').read_bytes()
    image = Image.new('RGB', (128, 64))
    image.putdata([INK if value else PAPER for value in raw])
    return image


def main(source=ROOT / 'build/host'):
    OUT.mkdir(parents=True, exist_ok=True)
    images = {name: load(source, name) for name in SCENES}
    for name, image in images.items():
        image.save(OUT / f'{name}-128x64.png')
        image.resize((768, 384), Image.Resampling.NEAREST).save(OUT / f'{name}.png')
    sheet = Image.new('RGB', (2 * 532 + 20, 5 * 276 + 20), (24, 38, 48))
    for index, name in enumerate(SCENES):
        x, y = 20 + index % 2 * 532, 20 + index // 2 * 276
        sheet.paste(images[name].resize((512, 256), Image.Resampling.NEAREST), (x, y))
    sheet.save(OUT / 'overview.png')
    print('Rendered production pet screens into', OUT)


if __name__ == '__main__':
    main(*sys.argv[1:])
