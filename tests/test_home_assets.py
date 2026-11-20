"""All shipped desktop frames must preserve both character ink and white fill."""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from make_home_assets import BANKS, CHARACTER_X, FRAME_SIZE, build_home_frames
from make_pet_room_concepts import opacity_mask, pack, room, unpack


class HomeAssetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = (ROOT / 'assets/whale-girl/whale_frames.bin').read_bytes()
        cls.atlas = (ROOT / 'firmware/flipper/applications/services/deepflipper/resources/deepflipper/home_frames.bin').read_bytes()

    def test_shipped_atlas_is_deterministic_and_has_two_complete_banks(self):
        manifest = json.loads((ROOT / 'assets/whale-girl/manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(len(self.atlas), manifest['unique_frame_count'] * FRAME_SIZE * BANKS)
        self.assertEqual(self.atlas, build_home_frames(self.raw))

    def test_all_composited_pixels_preserve_character_and_room(self):
        count = len(self.raw) // 512
        for index in range(count):
            character = unpack(self.raw[index * 512:(index + 1) * 512])
            mask = opacity_mask(character)
            for bank in range(BANKS):
                with self.subTest(frame=index, bank=bank):
                    offset = (bank * count + index) * FRAME_SIZE
                    actual = unpack(self.atlas[offset:offset + FRAME_SIZE], 128, 64)
                    background = room(night=bool(bank))
                    for y in range(64):
                        for x in range(64):
                            expected = (character.getpixel((x, y)) if mask.getpixel((x, y))
                                        else background.getpixel((CHARACTER_X + x, y)))
                            self.assertEqual(bool(actual.getpixel((CHARACTER_X + x, y))),
                                             bool(expected))
                    self.assertEqual(pack(actual.crop((0, 0, 32, 64))), pack(background.crop((0, 0, 32, 64))))

    def test_rejects_partial_character_frames(self):
        for data in (b'', bytes(511), bytes(513)):
            with self.assertRaises(ValueError):
                build_home_frames(data)


if __name__ == '__main__':
    unittest.main()
