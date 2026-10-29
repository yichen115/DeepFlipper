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

link_test=output/('expansion_link_test.exe' if sys.platform=='win32' else 'expansion_link_test')
with (output/'link-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                           '-I',str(ROOT),str(ROOT/'tests/expansion_link_test.cpp'),
                           '-o',str(link_test)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'link-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(link_test)],check=True)

component=ROOT/'firmware/esp32/managed_components/espressif__cjson/cJSON'
if not (component/'cJSON.c').is_file():
    raise SystemExit('Build/configure the ESP32 target once to fetch the managed cJSON component for native action tests.')
cjson=output/'cjson.o'
actions=output/('native_actions_test.exe' if sys.platform=='win32' else 'native_actions_test')
with (output/'actions-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'cc','-std=c99','-c',str(component/'cJSON.c'),'-o',str(cjson)],stdout=log,stderr=subprocess.STDOUT)
    if result.returncode==0:
        result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                               '-I',str(ROOT),'-I',str(component),str(ROOT/'tests/native_actions_test.cpp'),
                               str(cjson),'-o',str(actions)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'actions-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(actions)],check=True)
