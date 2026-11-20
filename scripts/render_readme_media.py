"""Record README preview GIFs from the production drawing code.

Run: uv run --with ziglang --with pillow python scripts/render_readme_media.py
Outputs assets/readme/*.gif. These are host renders of the real firmware
drawing functions (same fonts and SD art), not photos of a device.
"""
from pathlib import Path
import subprocess
import sys

from PIL import Image
import ziglang

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import host_fonts  # noqa: E402

ZIG = Path(ziglang.__file__).parent / ('zig.exe' if sys.platform == 'win32' else 'zig')
BUILD = ROOT / 'build/readme-media'
OUT = ROOT / 'assets/readme'
MEDIA = ROOT / 'scripts/readme_media'
RESOURCES = ROOT / 'firmware/flipper/applications/services/deepflipper/resources/deepflipper'
PET = ROOT / 'firmware/flipper/applications/examples/whale_pet'
PAPER, INK = (255, 167, 72), (36, 26, 16)
EXE = '.exe' if sys.platform == 'win32' else ''


def prefix(test, marker):
    text = (ROOT / 'tests' / test).read_text(encoding='utf-8')
    return text[:text.index(marker)]


def build_home():
    stubs = BUILD / 'desktop-stubs'
    for name in ('furi.h', 'storage/storage.h', 'gui/canvas.h', 'assets_icons.h', 'furi_hal_rtc.h', 'locale/locale.h'):
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('/* host boundary defined by the recorder */\n', encoding='utf-8')
    source = BUILD / 'home_record.c'
    source.write_text(prefix('home_sprite_test.c', 'static bool ink(') +
                      (MEDIA / 'home_record.c').read_text(encoding='utf-8'), encoding='utf-8')
    exe = BUILD / ('home_record' + EXE)
    subprocess.run([str(ZIG), 'cc', '-std=gnu11', '-w', '-I', str(stubs), '-I', str(ROOT), str(source),
                    str(PET / 'pet_model.c'), '-o', str(exe)], check=True)
    raw = BUILD / 'home.raw'
    subprocess.run([str(exe), str(RESOURCES / 'home_frames.bin'), str(raw)], check=True)
    data = raw.read_bytes()
    frames = []
    for offset in range(0, len(data), 1024):
        chunk = data[offset:offset + 1024]
        image = Image.new('RGB', (128, 64))
        image.putdata([INK if chunk[y * 16 + x // 8] >> (x % 8) & 1 else PAPER
                       for y in range(64) for x in range(128)])
        frames.append(image)
    return frames


def build_pet():
    source_text = (PET / 'whale_pet.c').read_text(encoding='utf-8')

    def extract(start, end, name):
        begin = source_text.index(start)
        (BUILD / name).write_text(source_text[begin:source_text.index(end, begin)], encoding='utf-8')

    extract('typedef struct {\n    WhalePetStore store;', '\nstatic uint32_t wall_time(', 'pet-app-struct-production.inc')
    extract('// ------------------------------------------------------------------ drawing',
            '// ------------------------------------------------------------------ input', 'pet-draw-production.inc')
    host_fonts.export(BUILD / 'pet_fonts.h')
    source = BUILD / 'pet_record.c'
    source.write_text(prefix('pet_render_test.c', 'int main(') +
                      (MEDIA / 'pet_record.c').read_text(encoding='utf-8'), encoding='utf-8')
    exe = BUILD / ('pet_record' + EXE)
    subprocess.run([str(ZIG), 'cc', '-std=gnu11', '-w', '-I', str(BUILD), '-I', str(PET), '-I', str(ROOT),
                    str(source), str(PET / 'pet_ui.c'), str(PET / 'pet_model.c'), '-o', str(exe)], check=True)
    raw = BUILD / 'pet.raw'
    subprocess.run([str(exe), str(RESOURCES / 'pet_frames.bin'), str(RESOURCES / 'pet_masks.bin'), str(raw)],
                   check=True)
    data = raw.read_bytes()
    frames = []
    for offset in range(0, len(data), 8192):
        image = Image.new('RGB', (128, 64))
        image.putdata([INK if value else PAPER for value in data[offset:offset + 8192]])
        frames.append(image)
    return frames


def save_gif(frames, name, ms, scale=4):
    big = [frame.resize((128 * scale, 64 * scale), Image.Resampling.NEAREST)
           .quantize(colors=2, method=Image.Quantize.MEDIANCUT) for frame in frames]
    big[0].save(OUT / name, save_all=True, append_images=big[1:], duration=ms, loop=0,
                optimize=True, disposal=1)
    print(f'{name}: {len(frames)} frames, {(OUT / name).stat().st_size // 1024} KiB')


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    home = build_home()
    save_gif(home, 'home.gif', 120)
    pet = build_pet()
    # Split the pet session into the care loop and the mini-game for the README.
    save_gif(pet[:90], 'pet-care.gif', 100)
    save_gif(pet[90:], 'pet-game.gif', 80)
    still = Image.new('RGB', (128 * 2 + 8, 64), (24, 38, 48))
    still.paste(home[10], (0, 0))
    still.paste(pet[40], (136, 0))
    still.resize((still.width * 4, 256), Image.Resampling.NEAREST).save(OUT / 'social-preview.png')


if __name__ == '__main__':
    main()
