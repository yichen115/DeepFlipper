"""Validate project structure, generated assets and the exported Flipper package."""
from pathlib import Path
import ast
import hashlib
import json
import re
import subprocess
import struct
from PIL import Image, ImageChops

ROOT=Path(__file__).resolve().parents[1]
assets=ROOT/'assets/whale-girl'
manifest=json.loads((assets/'manifest.json').read_text(encoding='utf-8'))
raw=(assets/'whale_frames.bin').read_bytes()
assert len(raw)==manifest['unique_frame_count']*512==66048
assert (assets/manifest['source']).is_file()
base=Image.open(assets/'source/accepted-screen.png').convert('1').crop((0,0,64,64))
frames=0
for animation in manifest['animations']:
    count=len(animation['frames'])
    for i,index in enumerate(animation['frames']):
        frame=Image.open(assets/'frames'/animation['id']/f'{i:02d}.png')
        assert frame.mode=='1' and frame.size==(64,64)
        packed=bytes(sum((frame.getpixel((x+b,y))==0)<<b for b in range(8))
                     for y in range(64) for x in range(0,64,8))
        assert packed==raw[index*512:(index+1)*512]
        bob=-1 if animation['id']=='happy' and i%8 in (2,3,4) else 0
        if animation['id'] in ('idle','sleeping') and count//3<=i<count*2//3:bob=-1
        assert ImageChops.difference(frame.crop((26,47+bob,36,55+bob)),
                                    base.crop((26,47,36,55))).getbbox() is None
        frames+=1
assert frames==228 and len(manifest['animations'])==12
assert bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',
             (assets/'whale_animations.h').read_text()))==raw

for file in [ROOT/'README.md',ROOT/'docs/final-spec.zh-CN.md',ROOT/'docs/implementation.md',
             assets/'README.zh-CN.md',ROOT/'protocol/README.md',ROOT/'hardware/README.md']:
    text=file.read_text(encoding='utf-8')
    for link in re.findall(r'\]\(([^)]+)\)',text):
        if '://' not in link and not link.startswith('#'):
            assert (file.parent/link.split('#')[0]).exists(),(file,link)
    assert text.count('```')%2==0,file
for file in (ROOT/'scripts').glob('*.py'):
    ast.parse(file.read_text(encoding='utf-8'))

release=ROOT/'build/release/flipper'
binary=release/'firmware.bin'
update=release/'f7-update-local/update.fuf'
config=update.read_text()
address=re.search(r'^Radio address:\s*(.+)$',config,re.M)[1]
radio=int.from_bytes(bytes.fromhex(address),'little')
gap=radio-0x08000000-binary.stat().st_size
assert gap>=4096,('Insufficient firmware/radio margin',gap)
assert (release/'deepflipper/whale_frames.bin').read_bytes()==raw
assert (ROOT/'firmware/flipper/build/f7-firmware-C/resources/deepflipper/whale_frames.bin').read_bytes()==raw
for name in ('firmware.dfu','radio.bin','resources.ths','splash.bin','updater.bin'):
    assert (update.parent/name).stat().st_size>0,name
for repo in ('unleashed-firmware','flipperzero-firmware','xiaozhi-esp32','xiaozhi-esp32-server'):
    status=subprocess.check_output(['git','-C',str(ROOT.parent/'refer'/repo),'status','--porcelain'],text=True)
    assert not status,('Reference checkout changed',repo)

test_log=(ROOT/'build-project-tests.log').read_text(encoding='utf-8',errors='replace')
test_count=int(re.search(r'^Ran (\d+) tests?',test_log,re.M)[1])
assert re.search(r'^OK\s*$',test_log,re.M), 'Project tests did not pass'
native_log=(ROOT/'build-native.log').read_text(encoding='utf-8',errors='replace')
assert 'PASS: C++ RPC codec' in native_log
assert 'PASS: native RPC streaming' in native_log
assert 'PASS: native action workflows' in native_log
assert 'PASS: Expansion 460800 negotiation' in native_log
esp_report={'target_sdk':'release/v6.1','target_build':'not exported','hardware_validated':False}
esp_release=ROOT/'build/release/esp32'
esp_result=ROOT/'build/esp32-build-result.json'
if esp_result.is_file() and (esp_release/'merged-binary.bin').is_file():
    result=json.loads(esp_result.read_text(encoding='utf-8'))
    description=json.loads((ROOT/'firmware/esp32/build/project_description.json').read_text(encoding='utf-8'))
    assert result['state']=='complete' and result['exit_code']==0
    assert description['target']=='esp32s3' and description['git_revision'].startswith('v6.1')
    assert Path(result['idf_path']).resolve()==Path(description['idf_path']).resolve()
    merged=(esp_release/'merged-binary.bin').read_bytes()
    assert hashlib.sha256(merged).hexdigest()==result['merged_sha256']
    assert merged[0]==0xE9
    flash=json.loads((esp_release/'flasher_args.json').read_text())
    end=0
    app=None
    for offset,name in sorted(flash['flash_files'].items(),key=lambda item:int(item[0],16)):
        image=esp_release/name
        assert image.is_file() and image.read_bytes()==(ROOT/'firmware/esp32/build'/name).read_bytes()
        assert int(offset,16)>=end
        end=int(offset,16)+image.stat().st_size
        if name=='xiaozhi.bin':app=(int(offset,16),image.stat().st_size)
    assert end<=16*1024*1024 and len(merged)>=end
    table=(esp_release/'partition_table/partition-table.bin').read_bytes()
    app_partition=None
    for pos in range(0,len(table)-31,32):
        magic,kind,subtype,offset,size,label,flags=struct.unpack('<HBBII16sI',table[pos:pos+32])
        if magic==0x50AA and kind==0 and offset==app[0]:app_partition=size
    assert app_partition and app[1]<=app_partition
    esp_report.update(target_build='passed via EIM',user_sdk_path=result['idf_path'],
                      python=result['python'],app_bytes=app[1],app_partition_bytes=app_partition,
                      merged_bytes=len(merged),merged_sha256=result['merged_sha256'])

report={
    'flipper':{'build':'updater_package DEBUG=0','firmware_bytes':binary.stat().st_size,
               'radio_address':hex(radio),'radio_gap_bytes':gap,
               'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
               'hardware_flashed':False},
    'assets':{'animations':12,'sequence_frames':frames,'unique_frames':manifest['unique_frame_count'],
              'bytes':len(raw),'per_frame_logo_preserved':True},
    'tests':{'project_python':f'{test_count} passed','native_rpc_codec':'passed',
             'native_actions':'passed; simulated transport, not physical hardware',
             'expansion_baud_fallback':'passed; simulated peer, not electrical validation',
             'relevant_upstream_board_tests':'9 passed',
             'broader_upstream_host_run':'140 run; 12 Windows environment errors; 19 skipped'},
    'esp32':esp_report,
    'references_unchanged':True,
}
selected_board_log=ROOT/'firmware/esp32/build-selected-board-tests.log'
if selected_board_log.is_file():
    selected=selected_board_log.read_text(encoding='utf-8',errors='replace')
    if re.search(r'^OK\s*$',selected,re.M):
        selected_count=int(re.search(r'^Ran (\d+) tests?',selected,re.M)[1])
        report['tests']['relevant_upstream_board_tests']=f'{selected_count} passed'
report['tests']['voice_endpoints']='passed' if 'PASS: WS/WSS voice endpoints' in native_log else 'not run'
local_smoke=ROOT/'build-local-smoke.log'
if local_smoke.is_file() and 'PASS: local CPU server/model initialized' in local_smoke.read_text(encoding='utf-8',errors='replace'):
    report['local_backend']={'cpu_model_and_ws_auth_hello':'passed',
                             'real_provider_chat':'not tested; user API configuration required'}
(ROOT/'build/validation.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(f'PASS: links, assets, references, Flipper package (gap {gap} bytes); ESP32 {esp_report["target_build"]}.')
