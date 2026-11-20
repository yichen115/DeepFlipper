"""Build metadata must come from the pinned protobuf archive, never parent Git tags."""
import ast
import os
from pathlib import Path
import re
import tempfile
import types
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'firmware/flipper/scripts/fbt_tools/fbt_assets.py'


class ProtobufVersionTests(unittest.TestCase):
    def generate(self, directory, invoke):
        tree = ast.parse(SOURCE.read_text(encoding='utf-8'))
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == '_proto_ver_generator')
        scope = {'os': os, 're': re, '__invoke_git': invoke,
                 'subprocess': types.SimpleNamespace(CalledProcessError=RuntimeError),
                 'StopError': RuntimeError}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(SOURCE), 'exec'), scope)
        target = directory / 'version.h'
        scope['_proto_ver_generator']([target], [types.SimpleNamespace(
            dir=types.SimpleNamespace(abspath=str(directory)))], {})
        return target.read_text()

    def test_archive_uses_changelog_without_git_or_network(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Changelog').write_text('# Changelog\n\n## [0.30]\n## [0.29]\n')
            invoke = Mock(side_effect=AssertionError('must not inspect parent Git'))
            result = self.generate(root, invoke)
            self.assertIn('PROTOBUF_MAJOR_VERSION 0', result)
            self.assertIn('PROTOBUF_MINOR_VERSION 30', result)
            invoke.assert_not_called()

    def test_missing_archive_metadata_fails_without_git(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, 'restore the pinned dependency'):
                self.generate(Path(directory), Mock(side_effect=AssertionError('must not use Git')))

    def test_real_nested_checkout_can_use_its_own_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / '.git').mkdir()
            invoke = Mock(side_effect=lambda args, **kwargs: '0.30' if args[0] == 'describe' else '')
            self.assertIn('PROTOBUF_MINOR_VERSION 30', self.generate(root, invoke))
