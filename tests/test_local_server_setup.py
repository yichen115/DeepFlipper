import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('local_setup', ROOT / 'scripts/setup_local_server.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class LocalSetupTests(unittest.TestCase):
    def test_repeat_setup_preserves_credentials_and_user_config(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'server').mkdir()
            template = root / 'server/deepflipper.local.config.example.yaml'
            template.write_text('url: ws://REPLACE_WITH_PC_IP:8000/xiaozhi/v1/\n', encoding='utf-8')
            config, key = setup.prepare('192.168.1.12', root)
            secret = key.read_bytes()
            values = dict(line.split(b'=', 1) for line in secret.splitlines() if b'=' in line)
            self.assertEqual(len(values[b'DEEPFLIPPER_AUTH_KEY']), 64)
            self.assertEqual(len(values[b'DEEPFLIPPER_ADMIN_TOKEN']), 64)
            self.assertEqual((config.parent / 'admin-token.txt').read_text().strip().encode(),
                             values[b'DEEPFLIPPER_ADMIN_TOKEN'])
            self.assertIn('ws://192.168.1.12:8000', config.read_text(encoding='utf-8'))
            config.write_text('custom: user-settings\n', encoding='utf-8')
            setup.prepare('192.168.1.13', root)
            self.assertEqual(key.read_bytes(), secret)
            self.assertEqual(config.read_text(encoding='utf-8'), 'custom: user-settings\n')

    def test_rejects_addresses_that_esp32_cannot_use(self):
        for host in ('127.0.0.1', '0.0.0.0', '::1', '::', 'ws://192.168.1.1', 'not-an-ip'):
            with self.subTest(host=host), self.assertRaises(ValueError):
                setup.prepare(host)
