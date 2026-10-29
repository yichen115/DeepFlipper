import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from download_utils import download, extract_source
from fetch_dependencies import fetch


def archive(path, files, links=()):
    with tarfile.open(path, 'w:gz') as out:
        for name, content in files.items():
            member = tarfile.TarInfo(name)
            member.size = len(content)
            out.addfile(member, io.BytesIO(content))
        for name, target in links:
            member = tarfile.TarInfo(name)
            member.type = tarfile.SYMTYPE
            member.linkname = target
            out.addfile(member)


class DependencyDownloadTests(unittest.TestCase):
    def test_extracts_portable_file_links_without_nested_git_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'source.tar.gz'
            archive(source, {'repo-commit/pb.h': b'header',
                             'repo-commit/.gitmodules': b'old metadata'},
                    [('repo-commit/headers/pb.h', '../pb.h')])
            output = root / 'source'
            extract_source(source, output)
            self.assertEqual((output / 'headers/pb.h').read_bytes(), b'header')
            self.assertFalse((output / '.gitmodules').exists())
            self.assertFalse((output / 'headers/pb.h').is_symlink())

    def test_rejects_archive_and_link_paths_outside_destination(self):
        for malicious in ('path', 'link'):
            with self.subTest(kind=malicious), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / 'source.tar.gz'
                if malicious == 'path':
                    archive(source, {'repo/../../escaped': b'bad'})
                else:
                    archive(source, {'repo/ok': b'ok'}, [('repo/link', '../../escaped')])
                with self.assertRaises(ValueError):
                    extract_source(source, root / 'source')
                self.assertFalse((root / 'escaped').exists())

    def test_existing_complete_dependency_is_not_overwritten_or_downloaded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'firmware/flipper/lib/example'
            source.mkdir(parents=True)
            (source / 'source.c').write_text('local changes', encoding='utf-8')
            dependency = {'path': 'lib/example', 'required': ['source.c']}
            with patch('fetch_dependencies.download', side_effect=AssertionError('must not download')):
                self.assertEqual(fetch(dependency, root), source)
            self.assertEqual((source / 'source.c').read_text(), 'local changes')

    def test_incomplete_dependency_and_mismatched_model_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'firmware/flipper/lib/example'
            source.mkdir(parents=True)
            custom = source / 'custom.c'
            custom.write_bytes(b'local')
            with self.assertRaises(ValueError):
                fetch({'path': 'lib/example', 'required': ['missing.c']}, root)
            self.assertEqual(custom.read_bytes(), b'local')
            with self.assertRaises(ValueError):
                download('https://unused.example/model', custom, '0' * 64)
            self.assertEqual(custom.read_bytes(), b'local')
