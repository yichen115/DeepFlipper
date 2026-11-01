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

speaker_test=output/('speaker_pcm_test.exe' if sys.platform=='win32' else 'speaker_pcm_test')
subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),
                str(ROOT/'tests/speaker_pcm_test.cpp'),'-o',str(speaker_test)],check=True)
subprocess.run([str(speaker_test)],check=True)

adapter=(ROOT/'firmware/flipper/applications/examples/deepflipper_tools/deepflipper_tools.c').read_text()
start=adapter.index('static void rpc_event(');end=adapter.index('static void detected(',start)
(output/'tool-rpc-production.inc').write_text(adapter[start:end],encoding='utf-8')
adapter_test=output/('tool_adapter_lifecycle.exe' if sys.platform=='win32' else 'tool_adapter_lifecycle')
subprocess.run([str(zig),'cc','-std=gnu11','-I',str(output),str(ROOT/'tests/tool_adapter_lifecycle.c'),'-o',str(adapter_test)],check=True)
subprocess.run([str(adapter_test)],check=True)

audio_service=(ROOT/'firmware/esp32/main/audio/audio_service.cc').read_text(encoding='utf-8')
begin=audio_service.index('bool AudioService::PushPcmToPlaybackQueue(')
end=audio_service.index('std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue(',begin)
(output/'pcm-queue-production.inc').write_text(audio_service[begin:end],encoding='utf-8')
application=(ROOT/'firmware/esp32/main/application.cc').read_text(encoding='utf-8')
begin=application.index('void Application::SuspendVoiceSession(')
(output/'preview-suspend-production.inc').write_text(application[begin:],encoding='utf-8')
for name in ('preview_session_test','pcm_queue_test','preview_suspend_test'):
    target=output/(name+('.exe' if sys.platform=='win32' else ''))
    subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),'-I',str(output),str(ROOT/'tests'/f'{name}.cpp'),'-o',str(target)],check=True)
    subprocess.run([str(target)],check=True)

link_test=output/('expansion_link_test.exe' if sys.platform=='win32' else 'expansion_link_test')
reference=output/'expansion-reference.o'
subprocess.run([str(zig),'cc','-std=gnu11','-I',str(ROOT),'-c',str(ROOT/'tests/expansion_reference.c'),'-o',str(reference)],check=True)
with (output/'link-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                           '-I',str(ROOT),str(ROOT/'tests/expansion_link_test.cpp'),
                           str(reference),'-o',str(link_test)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'link-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(link_test)],check=True)

# Compile the actual worker TX functions, not a host-side reimplementation.
worker=(ROOT/'firmware/flipper/applications/services/expansion/expansion_worker.c').read_text()
tx_begin=worker.index('static size_t\n    expansion_worker_send_callback(')
tx_end=worker.index('static bool expansion_worker_send_heartbeat(',tx_begin)
(output/'expansion-tx-production.inc').write_text(worker[tx_begin:tx_end],encoding='utf-8')
tx_reference=output/'expansion-tx-reference.o'
tx_test=output/('expansion_tx_test.exe' if sys.platform=='win32' else 'expansion_tx_test')
subprocess.run([str(zig),'cc','-std=gnu11','-I',str(ROOT),'-I',str(output),
                '-c',str(ROOT/'tests/expansion_tx_reference.c'),'-o',str(tx_reference)],check=True)
subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),
                str(ROOT/'tests/expansion_tx_test.cpp'),str(tx_reference),'-o',str(tx_test)],check=True)
subprocess.run([str(tx_test)],check=True)

component=ROOT/'firmware/esp32/managed_components/espressif__cjson/cJSON'
if not (component/'cJSON.c').is_file():
    raise SystemExit('Build/configure the ESP32 target once to fetch the managed cJSON component for native action tests.')
cjson=output/'cjson.o'
actions=output/('native_actions_test.exe' if sys.platform=='win32' else 'native_actions_test')
desktop=(ROOT/'firmware/flipper/applications/services/rpc/rpc_desktop.c').read_text()
begin=desktop.index('static void rpc_desktop_on_is_locked_request(')
end=desktop.index('static void rpc_desktop_on_unlock_request(',begin)
(output/'desktop-lock-production.inc').write_text(desktop[begin:end],encoding='utf-8')
desktop_reference=output/'desktop-reference.o'
subprocess.run([str(zig),'cc','-std=gnu11','-I',str(output),'-c',str(ROOT/'tests/desktop_reference.c'),'-o',str(desktop_reference)],check=True)
with (output/'actions-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'cc','-std=c99','-c',str(component/'cJSON.c'),'-o',str(cjson)],stdout=log,stderr=subprocess.STDOUT)
    if result.returncode==0:
        result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                               '-I',str(ROOT),'-I',str(component),str(ROOT/'tests/native_actions_test.cpp'),
                               str(cjson),str(desktop_reference),'-o',str(actions)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'actions-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(actions)],check=True)

portal=output/('portal_config_test.exe' if sys.platform=='win32' else 'portal_config_test')
with (output/'portal-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                           '-I',str(ROOT),'-I',str(component),str(ROOT/'tests/portal_config_test.cpp'),
                           str(cjson),'-o',str(portal)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'portal-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(portal)],check=True)

frozen=output/'frozen-reference.o'
interop=output/('companion_interop_test.exe' if sys.platform=='win32' else 'companion_interop_test')
with (output/'companion-compile.log').open('w',encoding='utf-8') as log:
    result=subprocess.run([str(zig),'cc','-std=gnu11','-I',str(ROOT),
                           '-c',str(ROOT/'tests/frozen_reference.c'),'-o',str(frozen)],
                          stdout=log,stderr=subprocess.STDOUT)
    if result.returncode==0:
        result=subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror',
                               '-I',str(ROOT),'-I',str(component),str(ROOT/'tests/companion_interop_test.cpp'),
                               str(frozen),str(cjson),'-o',str(interop)],stdout=log,stderr=subprocess.STDOUT)
if result.returncode:
    print((output/'companion-compile.log').read_text(encoding='utf-8')[-6000:])
    raise SystemExit(result.returncode)
subprocess.run([str(interop)],check=True)
