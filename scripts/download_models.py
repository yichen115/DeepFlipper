"""Download the pinned ASR/VAD/TTS models, keeping large files out of Git."""
import argparse
import json
from pathlib import Path
import shutil
import tarfile
import tempfile

from download_utils import download, sha256

ROOT = Path(__file__).resolve().parents[1]


def prepare_archive(model, check=False, root=ROOT):
    destination = (root / model['path']).resolve()
    if not destination.is_relative_to(root / 'server/main/xiaozhi-server/models'):
        raise ValueError('Model destination is outside the model directory')
    stamp = destination / '.deepflipper-files.json'
    if destination.exists():
        if not stamp.is_file():
            raise ValueError('Unmanaged TTS model directory preserved; move it aside before setup')
        files = json.loads(stamp.read_text())
        if files.get('archive_sha256') != model['sha256']:
            raise ValueError('TTS model version mismatch; existing model preserved')
        for name, expected in files['files'].items():
            target = (destination / name).resolve()
            if not target.is_relative_to(destination) or not target.is_file() or sha256(target) != expected:
                raise ValueError('TTS model missing or modified: ' + name)
        return
    if check:
        raise FileNotFoundError('Missing TTS model; run python scripts/download_models.py --tts-only')
    archive = download(model['url'], root / 'build/downloads' / (model['name'] + '.tar.bz2'), model['sha256'])
    staging_parent = root / 'build/model-staging'
    staging_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=staging_parent) as temporary:
        staging = Path(temporary) / model['name']
        staging.mkdir()
        hashes = {}
        with tarfile.open(archive, 'r:bz2') as source:
            for entry in source:
                parts = Path(entry.name).parts
                if not parts or parts[0] != model['name'] or '\\' in entry.name:
                    raise ValueError('Unexpected archive path')
                relative = Path(*parts[1:])
                target = (staging / relative).resolve()
                if not target.is_relative_to(staging.resolve()):
                    raise ValueError('Archive path escapes the model directory')
                if entry.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif entry.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(entry) as data, target.open('wb') as out:
                        shutil.copyfileobj(data, out)
                    hashes[relative.as_posix()] = sha256(target)
                else:
                    raise ValueError('Model archive contains unsupported links or special files')
        if not all((staging / name).exists() for name in ('model.onnx', 'lexicon.txt', 'tokens.txt', 'dict')):
            raise ValueError('TTS archive is incomplete')
        (staging / stamp.name).write_text(json.dumps({'archive_sha256': model['sha256'], 'files': hashes}),encoding='utf-8')
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging.replace(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='Check presence without downloading')
    parser.add_argument('--tts-only', action='store_true', help='Prepare only the CPU TTS model')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'models.lock.json').read_text(encoding='utf-8'))
    models = [] if args.tts_only else lock['files']
    for model in models:
        target = (ROOT / model['path']).resolve()
        if not target.is_relative_to(ROOT / 'server/main/xiaozhi-server/models'):
            raise ValueError('Model destination is outside the model directory')
        if args.check:
            if not target.is_file():
                raise SystemExit(f'Missing model file: {target}. Run python scripts/download_models.py')
        else:
            download(model['url'], target, model['sha256'])
        print(f"Ready: {model['path']}")
    for model in lock.get('archives', []):
        prepare_archive(model, args.check)
        print('Ready: ' + model['path'])
    print('PASS: selected local speech models prepared; models remain excluded from Git.')
