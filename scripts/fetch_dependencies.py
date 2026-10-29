"""Fetch pinned Flipper dependencies as ordinary, ignored source directories."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile

from download_utils import download, extract_source

ROOT = Path(__file__).resolve().parents[1]


def fetch(dependency, root=ROOT, cache=None, download_only=False):
    root = Path(root).resolve()
    destination = (root / 'firmware/flipper' / dependency['path']).resolve()
    if not destination.is_relative_to(root / 'firmware/flipper'):
        raise ValueError('Dependency path is outside Flipper sources')
    if not download_only and destination.is_dir():
        missing = [name for name in dependency['required'] if not (destination / name).is_file()]
        if missing and any(destination.iterdir()):
            raise ValueError(f'Incomplete dependency {destination}: {missing}. Existing files preserved; move the directory aside and retry.')
        if not missing:
            print(f"Ready: {dependency['path']}")
            return destination
    cache = Path(cache) if cache else root / 'build/downloads/dependencies'
    cache.mkdir(parents=True, exist_ok=True)
    name = dependency['path'].replace('/', '-') + '-' + dependency['commit'] + '.tar.gz'
    archive = download(dependency['archive'], cache / name, dependency.get('sha256'))
    if download_only:
        return archive
    with tempfile.TemporaryDirectory(prefix='extract-', dir=cache) as staging:
        extract_source(archive, staging)
        for name in dependency['required']:
            if not (Path(staging) / name).is_file():
                raise ValueError(f"Dependency archive lacks {name}: {dependency['path']}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_dir() and not any(destination.iterdir()):
            destination.rmdir()  # Empty placeholder from a parent dependency archive.
        if destination.exists():
            raise FileExistsError(f'Will not overwrite existing sources: {destination}')
        shutil.move(staging, destination)
    print(f"Installed: {dependency['path']} @ {dependency['commit'][:12]}")
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--download-only', action='store_true', help='Cache archives without touching source directories')
    parser.add_argument('--check', action='store_true', help='Check required files without downloading')
    parser.add_argument('--only', help='Dependency path from sources.lock.json')
    args = parser.parse_args()
    dependencies = json.loads((ROOT / 'sources.lock.json').read_text(encoding='utf-8'))['components']['flipper']['dependencies']
    if args.only:
        dependencies = [d for d in dependencies if d['path'] == args.only]
        if not dependencies:
            parser.error('Unknown dependency path')
    for dependency in dependencies:
        if args.check:
            missing = [n for n in dependency['required'] if not (ROOT / 'firmware/flipper' / dependency['path'] / n).is_file()]
            if missing:
                raise SystemExit(f"Missing dependency {dependency['path']}. Run python scripts/fetch_dependencies.py")
        else:
            fetch(dependency, download_only=args.download_only)
    print(f'PASS: {len(dependencies)} dependencies checked/prepared.')
