"""Prepare local Docker configuration without printing or overwriting credentials."""
import argparse
import ipaddress
import os
from pathlib import Path
import secrets

ROOT = Path(__file__).resolve().parents[1]


def prepare(host, root=ROOT):
    address = ipaddress.ip_address(host)
    if address.version != 4 or address.is_loopback or address.is_unspecified:
        raise ValueError('Use the PC LAN IPv4 address reachable by ESP32, not localhost or 0.0.0.0')
    authority = str(address)
    data = root / 'server/main/xiaozhi-server/data'
    data.mkdir(parents=True, exist_ok=True)
    config = data / '.config.yaml'
    if not config.exists():
        template = (root / 'server/deepflipper.local.config.example.yaml').read_text(encoding='utf-8')
        with config.open('x', encoding='utf-8') as out:
            out.write(template.replace('REPLACE_WITH_PC_IP', authority))
    key = data / 'server.env'
    if not key.exists():
        fd = os.open(key, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            out.write('DEEPFLIPPER_AUTH_KEY=' + secrets.token_hex(32) + '\n')
    values = dict(line.split('=', 1) for line in key.read_text(encoding='utf-8').splitlines()
                  if line and not line.startswith('#') and '=' in line)
    admin_token = values.get('DEEPFLIPPER_ADMIN_TOKEN', '')
    if not admin_token:
        admin_token = secrets.token_hex(32)
        with key.open('a', encoding='utf-8') as out:
            out.write('\nDEEPFLIPPER_ADMIN_TOKEN=' + admin_token + '\n')
    admin_file = data / 'admin-token.txt'
    if not admin_file.exists():
        fd = os.open(admin_file, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, 'w', encoding='utf-8') as out:
            out.write(admin_token + '\n')
    return config, key


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True, help='LAN IP address of this PC, reachable by ESP32')
    args = parser.parse_args()
    config, key = prepare(args.host)
    print(f'Local config: {config}')
    print(f'Persistent signing key file: {key} (value not printed)')
    print(f'Admin login token file: {config.parent / "admin-token.txt"} (value not printed)')
    print('Existing files preserved. Start the server, then open http://127.0.0.1:8001 to configure the model.')

