"""Check the delivered 1-bit SD art and opt-in pet registration."""
import importlib.util
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'assets/whale-pet'
APP = ROOT / 'firmware/flipper/applications/examples/whale_pet'


class PetAssetsTests(unittest.TestCase):
    def test_pack_has_native_frames_and_bounded_clip_indexes(self):
        manifest = json.loads((ART / 'manifest.json').read_text(encoding='utf-8'))
        data = (ART / 'pet_frames.bin').read_bytes()
        self.assertEqual((manifest['width'], manifest['height'], manifest['bytes_per_frame']), (64, 64, 512))
        self.assertEqual(len(data), manifest['unique_frame_count'] * 512)
        masks = (ART / 'pet_masks.bin').read_bytes()
        self.assertEqual(len(masks), len(data))
        self.assertTrue(all(ink & ~opaque == 0 for ink, opaque in zip(data, masks)))
        resources = ROOT / 'firmware/flipper/applications/services/deepflipper/resources/deepflipper'
        self.assertEqual((resources / 'pet_frames.bin').read_bytes(), data)
        self.assertEqual((resources / 'pet_masks.bin').read_bytes(), masks)
        self.assertEqual(len(manifest['animations']), 19)
        self.assertEqual(manifest['favorite_food'], 'white_rice')
        self.assertTrue((ART / manifest['food_reference']).is_file())
        self.assertTrue((ART / manifest['source']).is_file())
        names = set()
        for clip in manifest['animations']:
            self.assertNotIn(clip['id'], names)
            names.add(clip['id'])
            self.assertTrue(clip['frames'])
            self.assertTrue(all(0 <= i < manifest['unique_frame_count'] for i in clip['frames']))
            self.assertEqual(clip['tick_ms'], 100)
            self.assertTrue((ART / (clip['id'] + '.gif')).is_file())
        self.assertTrue({'rice', 'full', 'pat', 'sleeping', 'comfort', 'curious'} <= names)
        header = (APP / 'pet_art.h').read_text(encoding='ascii')
        self.assertEqual(int(re.search(r'#define PET_FRAME_COUNT (\d+)', header)[1]), manifest['unique_frame_count'])
        clips = (APP / 'pet_clips.h').read_text(encoding='ascii')
        self.assertEqual(len(re.findall(r'PetClip\w+,', clips)), 19)
        lengths = re.search(r'pet_clip_length\[PET_CLIP_COUNT\] = \{([^}]*)\}', clips)[1]
        self.assertEqual([int(x) for x in lengths.split(',')], [len(c['frames']) for c in manifest['animations']])
        # Day and night app rooms ship inside the FAP, 128x64 each.
        for name in ('pet_room_day', 'pet_room_night'):
            body = re.search(name + r'\[1024\] = \{([^}]*)\}', header)[1]
            self.assertEqual(len(re.findall(r'0x[0-9a-f]{2}', body)), 1024)

    def test_pet_registers_same_generic_sdk_without_new_esp32_actions(self):
        spec = importlib.util.spec_from_file_location('pet_catalog', ROOT / 'scripts/export_app_catalog.py')
        catalog = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(catalog)
        value = catalog.registration(APP / 'ai_app.json')
        self.assertEqual(value['app_id'], 'deepflipper.pet')
        self.assertEqual(value['app'], '/ext/apps/Games/whale_pet.fap')
        self.assertEqual(value['sdk'], 2)


if __name__ == '__main__':
    unittest.main()
