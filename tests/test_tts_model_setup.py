import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import download_models
from download_utils import sha256


class TtsModelSetupTests(unittest.TestCase):
    def archive(self,root,extra=''):
        path=root/'build/downloads/test.tar.bz2';path.parent.mkdir(parents=True)
        with tarfile.open(path,'w:bz2') as archive:
            for name in ('model.onnx','tokens.txt','lexicon.txt','dict/file',extra):
                if not name:continue
                data=b'test data';entry=tarfile.TarInfo('test/'+name);entry.size=len(data)
                archive.addfile(entry,io.BytesIO(data))
        return {'name':'test','path':'server/main/xiaozhi-server/models/test','url':'unused', 'sha256':sha256(path)}

    def test_repeat_setup_checks_files_and_preserves_modified_models(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);model=self.archive(root)
            download_models.prepare_archive(model,root=root)
            download_models.prepare_archive(model,check=True,root=root)
            file=root/model['path']/'tokens.txt';file.write_bytes(b'edited')
            with self.assertRaises(ValueError):download_models.prepare_archive(model,root=root)
            self.assertEqual(file.read_bytes(),b'edited')

    def test_archive_path_traversal_never_publishes_a_model(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);model=self.archive(root,'../../escape')
            with self.assertRaises(ValueError):download_models.prepare_archive(model,root=root)
            self.assertFalse((root/model['path']).exists())
