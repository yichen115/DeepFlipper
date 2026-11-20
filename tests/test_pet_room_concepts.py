"""The animated preview must retain the visible character after serialization."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('pet_room', ROOT / 'scripts/make_pet_room_concepts.py')
room = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(room)


class PetRoomConceptTests(unittest.TestCase):
    def test_every_character_ink_pixel_survives_the_packed_opacity_mask(self):
        raw = (ROOT / 'assets/whale-pet/pet_frames.bin').read_bytes()
        manifest = json.loads((ROOT / 'assets/whale-pet/manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(len(raw), manifest['unique_frame_count'] * 512)
        for index in range(manifest['unique_frame_count']):
            with self.subTest(frame=index):
                source = raw[index * 512:(index + 1) * 512]
                image = room.unpack(source)
                mask = room.opacity_mask(image)
                encoded = room.pack_mask(mask)
                self.assertEqual(len(encoded), 512)
                self.assertTrue(any(encoded), 'An empty mask makes the character disappear')
                self.assertTrue(all(ink & ~opaque == 0 for ink, opaque in zip(source, encoded)))
                ink = 0
                for y in range(64):
                    for x in range(64):
                        byte = y * 8 + x // 8
                        if encoded[byte] >> (x % 8) & 1:
                            self.assertTrue(mask.getpixel((x, y)))
                            if source[byte] >> (x % 8) & 1:
                                ink += 1
                self.assertGreater(ink, 500)

    def test_mode_one_mask_values_do_not_require_255(self):
        for value in (1, 255):
            mask = room.Image.new('1', (64, 64), value)
            self.assertEqual(room.pack_mask(mask), b'\xff' * 512)
        self.assertEqual(room.pack_mask(room.Image.new('1', (64, 64), 0)), b'\0' * 512)


if __name__ == '__main__':
    unittest.main()
