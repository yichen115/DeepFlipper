"""Run with: uv run --with ziglang python scripts/test_native.py"""
from pathlib import Path
import subprocess
import sys
import ziglang

ROOT=Path(__file__).resolve().parents[1]
zig=Path(ziglang.__file__).parent/('zig.exe' if sys.platform=='win32' else 'zig')
output=ROOT/'build/host'
output.mkdir(parents=True,exist_ok=True)
def extract(text, start, end, name):
    begin=text.index(start)
    (output/name).write_text(text[begin:text.index(end,begin)],encoding='utf-8')

rpc_source=(ROOT/'firmware/flipper/applications/services/rpc/rpc.c').read_text(encoding='utf-8')
extract(rpc_source,'typedef struct {\n    RpcSystemAlloc alloc;','\nstruct RpcSession', 'rpc-profile-production.inc')
extract(rpc_source,'RpcSession* rpc_session_open(', '\nvoid rpc_session_close(', 'rpc-open-production.inc')
extract(rpc_source,'static void rpc_session_thread_pending_callback(', '\nstatic void\n    rpc_session_thread_state_callback(', 'rpc-cleanup-production.inc')
extract(rpc_source,'static bool rpc_session_lock(', '\nstatic int32_t rpc_session_worker(', 'rpc-lock-production.inc')
extract(rpc_source,'bool rpc_local_app_lock(', '\nRpcOwner rpc_session_get_owner(', 'rpc-admission-production.inc')
expansion_source=(ROOT/'firmware/flipper/applications/services/expansion/expansion_worker.c').read_text(encoding='utf-8')
extract(expansion_source,'static void expansion_worker_rpc_terminated_callback(', '\nstatic bool expansion_worker_rpc_session_open(', 'expansion-terminated-production.inc')
extract(expansion_source,'static void expansion_worker_rpc_session_close(', '\nstatic bool expansion_worker_handle_state_handshake(', 'expansion-close-production.inc')
assistant=(ROOT/'firmware/flipper/applications/services/deepflipper/deepflipper.c').read_text(encoding='utf-8')
extract(assistant,'static void deepflipper_resume_uart(', '\nstatic int32_t worker(', 'assistant-resume-production.inc')
extract(assistant,'bool deepflipper_rpc_profile_enabled(', '\nstatic void fail(', 'assistant-profile-production.inc')
extract(assistant,'static void deepflipper_worker_start(', '\nvoid deepflipper_on_system_start(', 'assistant-worker-production.inc')
extract(assistant,'bool deepflipper_app_suspend_uart(', '\nbool deepflipper_prepare_app_start(', 'assistant-suspend-production.inc')
extract(assistant,'void deepflipper_set_settings_foreground(', '\nbool deepflipper_module_command(', 'assistant-finish-production.inc')
loader=(ROOT/'firmware/flipper/applications/services/loader/loader.c').read_text(encoding='utf-8')
extract(loader,'static bool loader_prepare_deepflipper(', '\nstatic LoaderMessageLoaderStatusResult loader_do_start_by_name(', 'loader-prepare-production.inc')
extract(loader,'static bool loader_caller_is_rpc(', '\nstatic LoaderMessageLoaderStatusResult loader_start_internal(', 'loader-caller-production.inc')
extract(loader,'static bool loader_deepflipper_keeps_link(', '\nstatic bool loader_prepare_deepflipper(', 'loader-keep-production.inc')
power_hal=(ROOT/'firmware/flipper/targets/f7/furi_hal/furi_hal_power.c').read_text(encoding='utf-8')
extract(power_hal,'void furi_hal_power_disable_otg(', '\nbool furi_hal_power_is_otg_enabled(', 'otg-hold-production.inc')
for name in ('rpc_memory_test','assistant_memory_test','otg_hold_test'):
    target=output/(name+('.exe' if sys.platform=='win32' else ''))
    subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(output),str(ROOT/'tests'/f'{name}.c'),'-o',str(target)],check=True)
    subprocess.run([str(target)],check=True)

# Compile real desktop input, animation acknowledgement and scene dispatch together.
desktop_source=ROOT/'firmware/flipper/applications/services/desktop'
extract((desktop_source/'views/desktop_view_main.c').read_text(encoding='utf-8'),
        'bool desktop_main_input_callback(', '\nDesktopMainView* desktop_main_alloc(', 'desktop-input-production.inc')
scene=(desktop_source/'scenes/desktop_scene_main.c').read_text(encoding='utf-8')
extract(scene, 'static inline bool desktop_scene_main_check_none(', '\nvoid desktop_scene_main_callback(', 'desktop-favorites-production.inc')
extract(scene, 'bool desktop_scene_main_on_event(', '\nvoid desktop_scene_main_on_exit(', 'desktop-scene-production.inc')
extract((desktop_source/'animations/animation_manager.c').read_text(encoding='utf-8'),
        'bool animation_manager_interact_process(', '\nstatic void animation_manager_start_new_idle(', 'desktop-animation-interact-production.inc')
extract((desktop_source/'animations/views/bubble_animation_view.c').read_text(encoding='utf-8'),
        'static bool bubble_animation_input_callback(', '\nstatic void bubble_animation_activate(', 'desktop-bubble-input-production.inc')
desktop_stubs=output/'desktop-stubs'
for name in ('furi.h', 'storage/storage.h', 'gui/canvas.h', 'assets_icons.h', 'furi_hal_rtc.h', 'locale/locale.h'):
    path=desktop_stubs/name
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text('/* Desktop host boundary is defined by the test. */\n',encoding='utf-8')
for name in ('desktop_pet_shortcut_test', 'home_sprite_test'):
    target=output/(name+('.exe' if sys.platform=='win32' else ''))
    # The home renderer reads the pet timeline written by the real app model.
    extra=[str(ROOT/'firmware/flipper/applications/examples/whale_pet/pet_model.c')] if name=='home_sprite_test' else []
    subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-I',str(desktop_stubs),'-I',str(ROOT),'-I',str(output),str(ROOT/'tests'/f'{name}.c'),
                    *extra,'-o',str(target)],check=True)
    arguments=[str(ROOT/'firmware/flipper/applications/services/deepflipper/resources/deepflipper/home_frames.bin'),str(output)] if name=='home_sprite_test' else []
    subprocess.run([str(target),*arguments],check=True)

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

adapter=(ROOT/'firmware/flipper/lib/deepflipper_app/deepflipper_app.c').read_text()
start=adapter.index('static void rpc_event(');end=adapter.index('\nbool deepflipper_app_cancelled(',start)
(output/'tool-rpc-production.inc').write_text(adapter[start:end],encoding='utf-8')
adapter_test=output/('tool_adapter_lifecycle.exe' if sys.platform=='win32' else 'tool_adapter_lifecycle')
subprocess.run([str(zig),'cc','-std=gnu11','-I',str(output),str(ROOT/'tests/tool_adapter_lifecycle.c'),'-o',str(adapter_test)],check=True)
subprocess.run([str(adapter_test)],check=True)

audio_service=(ROOT/'firmware/esp32/main/audio/audio_service.cc').read_text(encoding='utf-8')
extract(audio_service,'void AudioService::ResetDecoder(', '\nbool AudioService::IsPlaybackDrainedLocked(', 'network-playback-production.inc')
network_playback=output/('network_playback_test.exe' if sys.platform=='win32' else 'network_playback_test')
subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT),'-I',str(output),str(ROOT/'tests/network_playback_test.cpp'),'-o',str(network_playback)],check=True)
subprocess.run([str(network_playback)],check=True)
begin=audio_service.index('bool AudioService::PushPcmToPlaybackQueue(')
end=audio_service.index('std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue(',begin)
(output/'pcm-queue-production.inc').write_text(audio_service[begin:end],encoding='utf-8')
application=(ROOT/'firmware/esp32/main/application.cc').read_text(encoding='utf-8')
extract(application,'void Application::CancelSpeechStop(', '\nvoid Application::HandleStateChangedEvent(', 'speech-stop-production.inc')
speech_stop=output/('speech_stop_test.exe' if sys.platform=='win32' else 'speech_stop_test')
subprocess.run([str(zig),'c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(output),str(ROOT/'tests/speech_stop_test.cpp'),'-o',str(speech_stop)],check=True)
subprocess.run([str(speech_stop)],check=True)
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
    # A fresh checkout can run host tests without first installing the target SDK.
    import hashlib
    from urllib.request import urlopen
    component=ROOT/'build/host-deps/cJSON'
    component.mkdir(parents=True,exist_ok=True)
    hashes={
        'cJSON.c':'298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d',
        'cJSON.h':'25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee',
    }
    for name,digest in hashes.items():
        path=component/name
        if not path.is_file():
            path.write_bytes(urlopen('https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/'+name,timeout=45).read())
        if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise SystemExit('Host cJSON checksum mismatch: '+str(path))
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
    # The vendored formatter uses a null destination for its string sizing pass.
    result=subprocess.run([str(zig),'cc','-std=gnu11','-fno-sanitize=pointer-overflow','-I',str(ROOT),
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

# Compile and execute the whole production application SDK with deterministic I/O.
stubs=output/'app-sdk-stubs'
for name in ('furi.h','furi_hal.h','rpc/rpc_app.h','storage/storage.h'):
    path=stubs/name
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text('/* SDK host platform is defined in app_sdk_test.c. */\n',encoding='utf-8')
sdk=output/('app_sdk_test.exe' if sys.platform=='win32' else 'app_sdk_test')
adapter_source=(ROOT/'firmware/flipper/applications/examples/deepflipper_tools/deepflipper_tools.c').read_text(encoding='utf-8')
extract(adapter_source,'static const DeepFlipperAction actions[] = {','\nint32_t deepflipper_tools_main(', 'default-actions-production.inc')
subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(stubs),
                '-I',str(ROOT),'-I',str(output),'-I',str(ROOT/'firmware/flipper/lib'),
                str(ROOT/'tests/app_sdk_test.c'),str(frozen),'-o',str(sdk)],check=True)
subprocess.run([str(sdk)],check=True)

princeton=(ROOT/'firmware/flipper/lib/subghz/protocols/princeton.c').read_text(encoding='utf-8')
extract(princeton,'void subghz_protocol_decoder_princeton_feed(', '\nSubGhzProtocolStatus subghz_protocol_decoder_princeton_serialize(', 'princeton-feed-production.inc')
decoder=(ROOT/'firmware/flipper/lib/subghz/blocks/decoder.c').read_text(encoding='utf-8')
extract(decoder,'void subghz_protocol_blocks_add_bit(', '\nvoid subghz_protocol_blocks_add_to_128_bit(', 'princeton-bit-production.inc')
for name in ('flipper_format/flipper_format.h','subghz/receiver.h','subghz/subghz_protocol_registry.h'):
    path=stubs/name
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text('/* RF host platform is defined in subghz_analysis_test.c. */\n',encoding='utf-8')
analysis=output/('subghz_analysis_test.exe' if sys.platform=='win32' else 'subghz_analysis_test')
subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(stubs),
                '-I',str(ROOT),'-I',str(output),'-I',str(ROOT/'firmware/flipper/lib'),
                str(ROOT/'tests/subghz_analysis_test.c'),str(frozen),'-lm','-o',str(analysis)],check=True)
subprocess.run([str(analysis)],check=True)

radio_header=stubs/'subghz/devices/devices.h'
radio_header.parent.mkdir(parents=True,exist_ok=True)
radio_header.write_text('/* Radio boundary is defined in radio_session_test.c. */\n',encoding='utf-8')
radio=output/('radio_session_test.exe' if sys.platform=='win32' else 'radio_session_test')
subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(stubs),
                '-I',str(ROOT),'-I',str(ROOT/'firmware/flipper/lib'),
                str(ROOT/'tests/radio_session_test.c'),str(frozen),'-o',str(radio)],check=True)
subprocess.run([str(radio)],check=True)

# Assistant screen readback: production deepflipper_screen.c + frozen, stubbed GUI/Loader.
canvas_header=(ROOT/'firmware/flipper/applications/services/gui/canvas_i.h').read_text(encoding='utf-8')
extract(canvas_header,'/* DeepFlipper: on-demand capture','\n#ifdef __cplusplus\n}','canvas-text-production.inc')
screen=output/('screen_capture_test.exe' if sys.platform=='win32' else 'screen_capture_test')
subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT/'tests/screen-stubs'),
                '-I',str(output),'-I',str(ROOT/'firmware/flipper/lib'),
                str(ROOT/'tests/screen_capture_test.c'),str(frozen),'-o',str(screen)],check=True)
subprocess.run([str(screen)],check=True)

pet_sources=ROOT/'firmware/flipper/applications/examples/whale_pet'
for name in ('pet_model_test','pet_store_test','pet_ui_test'):
    target=output/(name+('.exe' if sys.platform=='win32' else ''))
    subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror',
                    '-I',str(stubs),'-I',str(ROOT),'-I',str(output),str(ROOT/'tests'/f'{name}.c'),
                    str(pet_sources/'pet_model.c'),str(pet_sources/'pet_ui.c'),'-o',str(target)],check=True)
    subprocess.run([str(target)],check=True)

# Rasterize the production pet drawing code with the firmware fonts and SD art.
pet_source=(pet_sources/'whale_pet.c').read_text(encoding='utf-8')
extract(pet_source,'typedef struct {\n    WhalePetStore store;','\nstatic uint32_t wall_time(','pet-app-struct-production.inc')
extract(pet_source,'// ------------------------------------------------------------------ drawing',
        '// ------------------------------------------------------------------ input','pet-draw-production.inc')
sys.path.insert(0,str(ROOT/'scripts'))
import host_fonts
host_fonts.export(output/'pet_fonts.h')
render=output/('pet_render_test.exe' if sys.platform=='win32' else 'pet_render_test')
subprocess.run([str(zig),'cc','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(output),'-I',str(pet_sources),
                '-I',str(ROOT),str(ROOT/'tests/pet_render_test.c'),str(pet_sources/'pet_ui.c'),
                str(pet_sources/'pet_model.c'),'-o',str(render)],check=True)
resources=ROOT/'firmware/flipper/applications/services/deepflipper/resources/deepflipper'
subprocess.run([str(render),str(resources/'pet_frames.bin'),str(resources/'pet_masks.bin'),str(output)],check=True)
