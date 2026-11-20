"""The post-update slideshow is one DeepFlipper page, without Unleashed links."""
from pathlib import Path
import sys
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from make_update_slide import SLIDE_DIR, SPRITE, render  # noqa: E402


class UpdateSlideTests(unittest.TestCase):
    def test_single_frame_matches_generator(self):
        frames = sorted(p.name for p in SLIDE_DIR.glob('frame_*.png'))
        self.assertEqual(frames, ['frame_00.png'])
        shipped = Image.open(SLIDE_DIR / 'frame_00.png').convert('1')
        self.assertEqual(shipped.size, (128, 64))
        self.assertEqual(shipped.tobytes(), render().tobytes())

    def test_original_sprite_kept_unchanged(self):
        sprite = Image.open(SPRITE).convert('1')
        self.assertEqual(render().crop((0, 0, 64, 64)).tobytes(), sprite.tobytes())


if __name__ == '__main__':
    unittest.main()
