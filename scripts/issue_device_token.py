"""Issue a device-bound token with the same persistent key used by the server."""
import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.auth import AuthManager

parser = argparse.ArgumentParser()
parser.add_argument('--device-id', required=True)
parser.add_argument('--client-id', required=True)
parser.add_argument('--output', type=Path, required=True, help='Private destination outside source control')
parser.add_argument('--key-file', type=Path, help='Local server.env signing key file; value is never printed')
args = parser.parse_args()
key = os.environ.get('DEEPFLIPPER_AUTH_KEY', '')
if args.key_file:
    entries = dict(line.split('=', 1) for line in args.key_file.read_text(encoding='utf-8').splitlines()
                   if line and not line.startswith('#') and '=' in line)
    key = entries.get('DEEPFLIPPER_AUTH_KEY', '')
if len(key) < 32:
    parser.error('Set DEEPFLIPPER_AUTH_KEY to the persistent server signing key')
if args.output.exists():
    parser.error('Output already exists; choose a new private destination')
token = AuthManager(key).generate_token(args.client_id, args.device_id)
with args.output.open('x', encoding='utf-8') as out:
    out.write(token + '\n')
print('Device credential written to the requested private file; no credential printed.')
