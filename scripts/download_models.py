"""Download only the ASR/VAD model files required by the local voice template."""
import argparse
import json
from pathlib import Path

from download_utils import download

ROOT = Path(__file__).resolve().parents[1]


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='Check presence without downloading')
    args = parser.parse_args()
    models = json.loads((ROOT / 'models.lock.json').read_text(encoding='utf-8'))['files']
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
    print(f'PASS: {len(models)} ASR/VAD model files prepared; models remain excluded from Git.')
