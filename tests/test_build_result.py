import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

p = Path(__file__).resolve().parents[1]/'scripts/esp32_build_entry.py'
spec = importlib.util.spec_from_file_location('esp32_build_entry', p)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BuildResultTests(unittest.TestCase):
    def test_real_failure_is_recorded_even_if_outer_runner_returns_success(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'result.json'
            code = module.run_build(path, 'this-build',
                                    runner=lambda *args, **kwargs: subprocess.CompletedProcess([], 7))
            result = json.loads(path.read_text())
            self.assertEqual(code, 7)
            self.assertEqual(result['nonce'], 'this-build')
            self.assertEqual(result['state'], 'failed')
            self.assertNotIn('merged_sha256', result)

    def test_runner_exception_cannot_leave_an_old_success_result(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'result.json'
            path.write_text('{"nonce":"old","state":"complete","exit_code":0}')
            def missing(*args, **kwargs):
                raise FileNotFoundError('build tool unavailable')
            self.assertEqual(module.run_build(path, 'new', runner=missing), 1)
            result = json.loads(path.read_text())
            self.assertEqual(result['nonce'], 'new')
            self.assertEqual(result['state'], 'failed')
