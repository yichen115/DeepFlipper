"""Install our versioned RPC extension into the downloaded protobuf sources."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
target = ROOT / 'firmware/flipper/assets/protobuf'
main = target / 'flipper.proto'
if not main.is_file():
    raise SystemExit('Missing firmware/flipper/assets/protobuf/flipper.proto; run python scripts/fetch_dependencies.py first.')
text = main.read_text(encoding='utf-8')
for tag, name in ((500, 'deepflipper_request'), (501, 'deepflipper_response')):
    import re
    for existing in re.findall(r'([^\n]+)=\s*' + str(tag) + r'\s*;', text):
        if name not in existing:
            raise SystemExit(f'RPC tag {tag} already used: {existing}')
if 'import "deepflipper.proto";' not in text:
    text = text.replace('syntax = "proto3";', 'syntax = "proto3";\nimport "deepflipper.proto";', 1)
if 'deepflipper_request = 500' not in text:
    text = text.replace('oneof content {', 'oneof content {\n'
                        '        .PB_DeepFlipper.Envelope deepflipper_request = 500;\n'
                        '        .PB_DeepFlipper.Envelope deepflipper_response = 501;', 1)
main.write_text(text, encoding='utf-8', newline='\n')
for name in ('deepflipper.proto', 'deepflipper.options'):
    shutil.copyfile(ROOT / 'protocol' / name, target / name)
print('DeepFlipper protocol synchronized (PB.Main 500/501).')
