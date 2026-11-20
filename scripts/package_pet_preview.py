"""Export a small, local SD preview without including existing pet saves."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = ('apps/Games/whale_pet.fap', 'deepflipper/pet_frames.bin',
         'deepflipper/apps/deepflipper.pet.json', 'deepflipper/pet_masks.bin')
NOTES = """Whale Pet - local SD preview

Requires the matching DeepFlipper Flipper firmware SDK 88.25.
Copy apps/ and deepflipper/ to the SD root, merging directories. Then open
Applications > Games > Whale Pet. This archive contains no pet save files.

Left/Right: choose an action in the bottom menu (Pat, Feed, Play, Catch,
Sleep, Stats; Wake while she sleeps). OK: do it. Hold OK: stats.
On the stats page Left/Right opens the expression gallery.
Catch: Catch Rice mini-game, hold Left/Right to move the bowl.
Back: return/exit. Hold Back: exit from any pet page.

Manual pet UI pauses the existing AI UART connection. Voice resumes on exit.
The generic SDK pet.state/pet.interact calls save care state without opening
the pet screen (displayed:false). Live voice inside the pet UI is not included.
No battle or multiplayer. Not validated on physical hardware.
"""


def package(release):
    release = Path(release)
    payload = {name: (release / name).read_bytes() for name in FILES}
    fap = payload[FILES[0]]
    registration = json.loads(payload[FILES[2]])
    if (not fap.startswith(b'\x7fELF') or registration['app_id'] != 'deepflipper.pet'
            or registration['sdk'] != 2 or registration['app'] != '/ext/' + FILES[0]
            or registration['fap_size'] != len(fap)
            or registration['fap_md5'] != hashlib.md5(fap).hexdigest()):
        raise ValueError('Pet registration does not match the exported FAP')
    manifest = json.loads((ROOT / 'assets/whale-pet/manifest.json').read_text(encoding='utf-8'))
    if len(payload[FILES[1]]) != 512 * manifest['unique_frame_count']:
        raise ValueError('Pet frame pack size differs from the generated manifest')
    if payload[FILES[1]] != (ROOT / 'assets/whale-pet/pet_frames.bin').read_bytes():
        raise ValueError('Pet frame pack is stale')
    if payload[FILES[3]] != (ROOT / 'assets/whale-pet/pet_masks.bin').read_bytes():
        raise ValueError('Pet mask pack is stale')
    payload['INSTALL.txt'] = NOTES.encode('utf-8')
    payload['SHA256SUMS.txt'] = ''.join(
        f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(payload.items())
    ).encode('ascii')
    destination = release / 'WhalePet-sd-preview.zip'
    temporary = destination.with_suffix('.zip.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() is not None:
            raise ValueError('Pet preview ZIP verification failed')
    temporary.replace(destination)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-dir', type=Path, default=ROOT / 'build/release/flipper')
    args = parser.parse_args()
    print(package(args.release_dir))
