"""Catalog packaging binds a strict opt-in registration to the actual FAP."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('export_app_catalog', ROOT / 'scripts/export_app_catalog.py')
catalog = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(catalog)


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = self.root / 'ai_app.json'
        self.binary = self.root / 'test.fap'
        self.binary.write_bytes(b'\x7fELFtest-app')
        self.value = {'format': 'deepflipper.app', 'v': 1, 'sdk': 2,
                      'app_id': 'test.app', 'name': 'Test App', 'summary': 'Reads test state.',
                      'app': '/ext/apps/Tools/test.fap'}
        self.write()

    def write(self):
        self.manifest.write_text(json.dumps(self.value, ensure_ascii=False), encoding='utf-8')

    def export(self):
        return catalog.export(self.manifest, self.binary, self.root / 'catalog')

    def test_registration_matches_artifact_and_changes_when_fap_changes(self):
        path = self.export()
        self.assertEqual(path.name, 'test.app.json')
        value = json.loads(path.read_bytes())
        self.assertEqual(value['fap_size'], self.binary.stat().st_size)
        self.assertEqual(value['fap_md5'], hashlib.md5(self.binary.read_bytes()).hexdigest())
        self.assertLessEqual(path.stat().st_size, 767)
        self.assertFalse(path.with_suffix('.json.tmp').exists())
        self.binary.write_bytes(b'\x7fELFnew-app')
        self.assertNotEqual(json.loads(self.export().read_bytes())['fap_md5'], value['fap_md5'])

    def test_path_and_id_are_canonical(self):
        for key, values in (
            ('app', ['/ext/apps/../test.fap', '/ext/apps//test.fap', '/ext/apps/Tools/a\\test.fap',
                     '/ext/apps/Tools/test.fap\n', '/ext/nfc/test.fap', '/ext/apps/C:/test.fap']),
            ('app_id', ['', '../escape', '-hidden', 'Upper.App', 'a' * 49]),
        ):
            original = self.value[key]
            for value in values:
                with self.subTest(key=key, value=value):
                    self.value[key] = value
                    self.write()
                    with self.assertRaises(ValueError):
                        self.export()
            self.value[key] = original

    def test_invalid_schema_and_mismatched_binary_preserve_existing_registration(self):
        path = self.export()
        original = path.read_bytes()
        for field, value in (('sdk', True), ('v', True), ('sdk', 3), ('name', 'x' * 65),
                             ('summary', '中' * 54), ('format', 'other'), ('app', '/ext/apps/other.fap')):
            with self.subTest(field=field):
                saved = self.value[field]
                self.value[field] = value
                self.write()
                with self.assertRaises(ValueError):
                    self.export()
                self.assertEqual(path.read_bytes(), original)
                self.value[field] = saved
        self.write()
        self.binary.write_bytes(b'not an ELF file')
        with self.assertRaises(ValueError):
            self.export()
        self.assertEqual(path.read_bytes(), original)

    def test_project_registration_matches_default_adapter(self):
        value = catalog.registration(ROOT / 'firmware/flipper/applications/examples/deepflipper_tools/ai_app.json')
        self.assertEqual(value['app_id'], 'deepflipper.tools')
        self.assertEqual(value['app'], '/ext/apps/Tools/deepflipper_tools.fap')

    def test_duplicate_fields_are_rejected(self):
        data = self.manifest.read_text(encoding='utf-8')
        self.manifest.write_text(data.replace('{', '{"v":1,', 1), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Duplicate registration field'):
            self.export()


if __name__ == '__main__':
    unittest.main()
