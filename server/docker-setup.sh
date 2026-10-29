#!/bin/sh
# DeepFlipper voice-only deployment; no upstream stock images or web resource generator.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ ! -f main/xiaozhi-server/data/.config.yaml ] || [ ! -f main/xiaozhi-server/data/server.env ] || [ ! -f main/xiaozhi-server/data/admin-token.txt ]; then
    echo "Prepare the local config first: python scripts/setup_local_server.py --host YOUR_LAN_IP (from the project root)." >&2
    echo "Set the DeepSeek model/API key and run python scripts/download_models.py. See ../README.md." >&2
    exit 1
fi
docker compose -f compose.local.yaml up -d --build
