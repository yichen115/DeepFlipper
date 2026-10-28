"""Run inside EIM and report the real build status, independently of its exit code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run_build(result_path, nonce, runner=subprocess.run):
    result = {'nonce': nonce, 'state': 'failed', 'exit_code': 1,
              'idf_path': os.environ.get('IDF_PATH', ''), 'python': sys.executable}
    try:
        process = runner([sys.executable, 'scripts/build.py', 'deepflipper-s3',
                          '--name', 'deepflipper-s3'], cwd=ROOT/'firmware/esp32')
        result['exit_code'] = process.returncode
        if process.returncode == 0:
            merged = ROOT/'firmware/esp32/build/merged-binary.bin'
            result['merged_sha256'] = hashlib.sha256(merged.read_bytes()).hexdigest()
            result['state'] = 'complete'
    except Exception as error:
        result['error'] = str(error)
        result['exit_code'] = 1
    result_path = Path(result_path)
    result_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = result_path.with_suffix('.tmp')
    temporary.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    temporary.replace(result_path)
    return result['exit_code']


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--result', type=Path, required=True)
    parser.add_argument('--nonce', required=True)
    arguments = parser.parse_args()
    raise SystemExit(run_build(arguments.result, arguments.nonce))
