"""Bind an opt-in SDK app registration to the built FAP for SD discovery.

The catalog is metadata only: action schemas are always obtained from the app's
live describe action. MD5 binds the registration to the installed file using
Flipper's native Storage RPC; it is not a signature or a trust decision.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re


def unique_fields(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('Duplicate registration field: ' + key)
        value[key] = item
    return value


def registration(source):
    value = json.loads(Path(source).read_text(encoding='utf-8'), object_pairs_hook=unique_fields)
    keys = {'format', 'v', 'app_id', 'name', 'summary', 'app', 'sdk'}
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError('Registration requires format, v, app_id, name, summary, app and sdk')
    if value['format'] != 'deepflipper.app' or type(value['v']) is not int or value['v'] != 1:
        raise ValueError('Unsupported catalog format/version')
    if type(value['sdk']) is not int or value['sdk'] != 2:
        raise ValueError('Registration requires SDK 2')
    if not isinstance(value['app_id'], str) or not re.fullmatch(r'[a-z0-9][a-z0-9._-]{0,47}', value['app_id']):
        raise ValueError('app_id must use 1-48 lowercase ASCII identifier characters')
    for name, maximum in (('name', 64), ('summary', 160), ('app', 192)):
        text = value[name]
        if (not isinstance(text, str) or not text or len(text.encode('utf-8')) > maximum
                or any(ord(c) < 32 or ord(c) == 127 for c in text)):
            raise ValueError(f'Invalid registration {name}')
    path = value['app']
    parts = path.split('/')
    if (not path.startswith('/ext/apps/') or not path.endswith('.fap') or '\\' in path
            or ':' in path or any(part in ('', '.', '..') for part in parts[1:])):
        raise ValueError('App must be a canonical /ext/apps/...fap path')
    return value


def export(manifest, binary, destination):
    value = registration(manifest)
    binary = Path(binary)
    if binary.name != PurePosixPath(value['app']).name:
        raise ValueError('Registration path does not name the built FAP')
    size = binary.stat().st_size
    if not 4 <= size <= 1024 * 1024:
        raise ValueError('Catalog FAP size must be 4 bytes through 1 MiB')
    content = binary.read_bytes()
    if not content.startswith(b'\x7fELF'):
        raise ValueError('Built FAP is not an ELF artifact')
    value['fap_size'] = size
    value['fap_md5'] = hashlib.md5(content, usedforsecurity=False).hexdigest()
    encoded = (json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')
    if len(encoded) > 767:
        raise ValueError('Catalog registration exceeds 767 UTF-8 bytes')
    output = Path(destination) / (value['app_id'] + '.json')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.json.tmp')
    temporary.write_bytes(encoded)
    temporary.replace(output)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--fap', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    print(export(args.manifest, args.fap, args.output_dir))


if __name__ == '__main__':
    main()
