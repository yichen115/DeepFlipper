"""Installation downloads and selecting completed training exports (no GPU or network)."""
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "singing"))
from train_voice import exported_models


class TrainingExportTests(unittest.TestCase):
    def test_selects_highest_epoch_not_filename_or_training_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("whale_25e_900s.pth", "whale_100e_3600s.pth", "G_999999.pth",
                         "whale_bad.pth", "other_300e_9000s.pth"):
                with (root / name).open("wb") as stream:
                    stream.truncate(1_000_001)
            (root / "whale_300e_9999s.pth").write_bytes(b"incomplete")
            models = exported_models(root, "whale")
            self.assertEqual([m[0] for m in models], [25, 100])
            self.assertEqual(models[-1][2].name, "whale_100e_3600s.pth")


@unittest.skipUnless(importlib.util.find_spec("yaml"), "PyYAML required")
class ModelDownloadTests(unittest.TestCase):
    class Response(io.BytesIO):
        def __init__(self, data, status=200, **headers):
            super().__init__(data)
            self.status = status
            self.headers = headers

    def test_small_json_is_validated_and_reused(self):
        from fetch_models import download
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "config.json"
            target.write_text('{"hidden_size": 768}', encoding="utf-8")
            with patch("fetch_models.urllib.request.urlopen") as network:
                download("https://example.invalid/model", str(target))
            network.assert_not_called()

    def test_interrupted_download_resumes_and_publishes_atomically(self):
        from fetch_models import download
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "model.bin"
            Path(str(target) + ".part").write_bytes(b"abc")
            response = self.Response(b"def", 206, **{"Content-Range": "bytes 3-5/6", "Content-Length": "3"})
            with patch("fetch_models.urllib.request.urlopen", return_value=response) as network:
                download("https://example.invalid/model", str(target))
            self.assertEqual(network.call_args.args[0].get_header("Range"), "bytes=3-")
            self.assertEqual(target.read_bytes(), b"abcdef")

    def test_server_ignoring_range_replaces_partial_instead_of_appending(self):
        from fetch_models import download
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "model.bin"
            Path(str(target) + ".part").write_bytes(b"old")
            with patch("fetch_models.urllib.request.urlopen", return_value=self.Response(b"new", 200)):
                download("https://example.invalid/model", str(target))
            self.assertEqual(target.read_bytes(), b"new")

    def test_invalid_json_is_not_published_or_resumed(self):
        from fetch_models import download
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "config.json"
            responses = [self.Response(b"<html>bad gateway</html>"), self.Response(b'{"ok":true}')]
            with patch("fetch_models.urllib.request.urlopen", side_effect=responses) as network, \
                 patch("fetch_models.time.sleep"):
                download("https://example.invalid/config", str(target))
            self.assertIsNone(network.call_args.args[0].get_header("Range"))
            self.assertEqual(target.read_text(), '{"ok":true}')


if __name__ == "__main__":
    unittest.main()
