"""Run with: uv run --with ziglang python scripts/test_native.py"""
from pathlib import Path
import subprocess
import sys
import ziglang

ROOT=Path(__file__).resolve().parents[1]
zig=Path(ziglang.__file__).parent/('zig.exe' if sys.platform=='win32' else 'zig')
output=ROOT/'build/host'
output.mkdir(parents=True,exist_ok=True)
binary=output/('rpc_wire_test.exe' if sys.platform=='win32' else 'rpc_wire_test')
with (output/'compile.log').open('w',encoding='utf-8') as log:
    compiled=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                            '-Wno-nullability-completeness','-I',str(ROOT),
                            str(ROOT/'tests/rpc_wire_test.cpp'),'-o',str(binary)],
                            stdout=log,stderr=subprocess.STDOUT)
if compiled.returncode:
    print((output/'compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(compiled.returncode)
subprocess.run([str(binary)],check=True)
