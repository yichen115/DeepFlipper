"""Render the real desktop frames captured by the native renderer test.

The firmware now draws time, battery, bubbles and the lock into the frame
buffer itself, so each capture is exactly the 128x64 picture on the LCD.
"""
from pathlib import Path
import sys
from PIL import Image
from make_pet_room_concepts import unpack

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets/whale-pet/home'
NAMES = ('ready', 'night', 'listening', 'pet-sleeping', 'locked', 'fallback')


def main(source=ROOT / 'build/host'):
    OUT.mkdir(parents=True, exist_ok=True)
    for name in NAMES:
        native = unpack((Path(source) / f'home-{name}.draw').read_bytes()[:1024], 128, 64)
        native.save(OUT / f'{name}-128x64.png')
        color = Image.new('RGB', native.size)
        color.putdata([(239, 182, 72) if pixel else (41, 31, 23) for pixel in native.getdata()])
        color.resize((768, 384), Image.Resampling.NEAREST).save(OUT / f'{name}.png')
    sheet = Image.new('RGB', (2 * 532 + 20, 3 * 276 + 20), (24, 38, 48))
    for index, name in enumerate(NAMES):
        tile = Image.open(OUT / f'{name}.png').resize((512, 256), Image.Resampling.NEAREST)
        sheet.paste(tile, (20 + index % 2 * 532, 20 + index // 2 * 276))
    sheet.save(OUT / 'overview.png')
    print('Rendered actual desktop frames into', OUT)


if __name__ == '__main__':
    main(*sys.argv[1:])
