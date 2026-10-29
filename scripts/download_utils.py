"""Checked, atomic downloads shared by dependency and model setup commands."""
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, target, expected=None):
    target = Path(target)
    if target.is_file():
        if expected and sha256(target) != expected:
            raise ValueError(f'Checksum mismatch: {target}. Existing file preserved; move it aside before retrying.')
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + '.download')
    print(f'Downloading {target.name}', flush=True)
    request = urllib.request.Request(url, headers={'User-Agent': 'DeepFlipper-source-setup/1'})
    with urllib.request.urlopen(request, timeout=60) as source, partial.open('wb') as out:
        shutil.copyfileobj(source, out, length=1024 * 1024)
    if expected and sha256(partial) != expected:
        raise ValueError(f'Checksum mismatch in download: {partial}; destination was not changed.')
    partial.replace(target)
    return target


def extract_source(archive_path, destination):
    """Extract a GitHub source archive, stripping its prefix and Git metadata.

    Materialize in-tree file links so downloaded sources also work on Windows
    without Developer Mode. Paths and links cannot escape the staging directory.
    """
    root = Path(destination).resolve()
    links = []
    with tarfile.open(archive_path, 'r:gz') as archive:
        prefix = None
        for entry in archive:
            parts = Path(entry.name).parts
            if not parts:
                continue
            if prefix is None:
                prefix = parts[0]
            if parts[0] != prefix or entry.name.startswith('/'):
                raise ValueError('Archive has an invalid source prefix')
            relative = Path(*parts[1:])
            if any(part in ('.git', '.gitmodules') for part in relative.parts):
                continue
            target = (root / relative).resolve()
            if not target.is_relative_to(root):
                raise ValueError('Archive path escapes the source directory')
            if entry.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif entry.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(entry) as source, target.open('wb') as out:
                    shutil.copyfileobj(source, out)
                target.chmod(entry.mode & 0o777)
            elif entry.issym() or entry.islnk():
                source = (target.parent / entry.linkname).resolve() if entry.issym() else (
                    root / Path(*Path(entry.linkname).parts[1:])).resolve()
                if not source.is_relative_to(root):
                    raise ValueError('Archive link escapes the source directory')
                links.append((target, source))
            else:
                raise ValueError(f'Unsupported archive entry: {entry.name}')
    for target, source in links:
        if not source.is_file():
            raise ValueError(f'Archive link target is not a regular source file: {source}')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

