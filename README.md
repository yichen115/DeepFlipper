# DeepFlipper

Whale-girl system mascot and voice assistant for Flipper Zero, based on Unleashed,
with a detachable ESP32-S3 audio module and a XiaoZhi-compatible DeepSeek backend.
The device UI is English. Conversation is spoken; there are no chat captions.

## Project layout

```text
firmware/flipper/   Unleashed fork: desktop, system service, RPC, settings and NFC job
firmware/esp32/     XiaoZhi fork: new deepflipper-s3 board, UART bridge, I2S audio, RGB
server/            XiaoZhi server fork; Python backend is main/xiaozhi-server/
protocol/          Canonical bounded RPC envelope and wire protocol documentation
assets/whale-girl/  Approved landscape character, 12 animation sequences and source
hardware/          Reference wiring and unresolved component choices; not a PCB
scripts/           Protocol/assets export, Flipper build and device-token utility
tests/             Authentication, asynchronous jobs and native C++ RPC codec tests
docs/              One final specification and the implementation status
```

The three component repositories have independent `deepflipper` development
branches and retain their upstream history/licenses. `refer/` was not modified.
See [source revisions](sources.lock.json), [final specification](docs/final-spec.zh-CN.md)
and [implementation/validation status](docs/implementation.md).

## Implemented

- Built-in mascot renderer with bounded SD frame reads and a firmware fallback.
- Resident Flipper assistant state, UART-only system RPC extension, configuration
  mailbox and application admission that stops the assistant's NFC worker first.
- English **Settings -> AI Assistant**, including masked secrets and an opt-in
  punctuation keyboard (hold UP to change page; long OK changes case).
- ESP32-S3 reference board: I2S microphone/speaker, PWM RGB, talk button, direct
  WS/WSS backend, persistent settings, actual PCM-derived mouth levels, device MCP.
- Flipper status and bounded NFC protocol detection jobs, with query/cancel and
  server-side asynchronous polling for the actual result.
- Device-bound signed tokens, mandatory validation on voice/configuration routes,
  enrollment/revocation lists and a private-file credential issuance utility.

## Build Flipper

From this project directory, in PowerShell:

```powershell
python scripts/sync_protocol.py
python scripts/export_flipper_assets.py
./scripts/build_flipper.ps1
# Full SD/update package, including original applications and resources:
./scripts/build_flipper.ps1 -Package
```

The build script initializes the original Flipper submodules as needed. Its
toolchain is separate from ESP-IDF. Output is under `build/release/flipper/`.
Nothing is flashed automatically. Install the matched SD resources as well as
the firmware: animations are in `/ext/deepflipper/whale_frames.bin`.

## ESP32

Target SDK: **ESP-IDF release/v6.1**, managed by the user with EIM. No project
script installs ESP-IDF or changes the existing IDF 5.3.2 installation.
The registered EIM environment `v6.1` has been verified and used to build the
module, with SDK `D:\esp\v6.1\esp-idf` and tools in `D:\Espressif\tools`.
From a normal PowerShell terminal:

```powershell
./scripts/build_esp32.ps1
# Or supply a different EIM executable/location:
./scripts/build_esp32.ps1 -EimPath E:/MyWork/DeepFlipper/eim-cli.exe -Idf v6.1
```

The script checks the actual child build result and image hash before exporting
to `build/release/esp32/`, including every file referenced by the flash manifest.
Inside an already activated IDF 6.1 environment the canonical command is:

```text
cd firmware/esp32
python scripts/build.py deepflipper-s3 --name deepflipper-s3
```

The reference variant uses button-controlled half-duplex audio; wake-word/AFE
features are disabled until their models and hardware are qualified. Before
flashing, verify the [reference pin map](hardware/README.md) against the actual
board. Target compilation has passed; hardware validation remains pending.

## Backend and provisioning

The Python backend entry is `server/main/xiaozhi-server/app.py`. Start from
`server/deepflipper.config.example.yaml`, copy it to that backend's
`data/.config.yaml`, and supply provider settings locally. The original
requirements include the selected speech providers; see the upstream server README.

Set a stable `DEEPFLIPPER_AUTH_KEY` of at least 32 characters in the server process
environment. It is a server signing secret; never enter it on the module. Expose
public voice endpoint through TLS, for example the [Caddy configuration](server/Caddyfile.example).
For a trusted LAN, an explicit `ws://PC_IP:8000/xiaozhi/v1/` URL works without a
domain or certificate. Device tokens remain mandatory; WS does not encrypt them.

Read the module's Device ID and Client ID in **AI Assistant** settings, then issue
a device credential on the trusted server/admin machine:

```text
python scripts/issue_device_token.py --device-id DEVICE_MAC --client-id CLIENT_UUID --output PRIVATE_TOKEN_FILE
```

Enter the WS/WSS URL and issued device token in Flipper settings. Only the ESP32
stores credentials; normal query responses do not disclose them. A first-time
module can be configured over UART before Wi-Fi is connected, or via the phone
Wi-Fi setup action. Enable the Flipper AI session and physically turn the module
on. The AI session switch is not a substitute for the physical power switch.

## Local Windows backend (Docker Desktop)

From this project directory, prepare a local configuration (replace the IP with
this PC's LAN address, reachable from the ESP32 Wi-Fi):

```powershell
python scripts/setup_local_server.py --host 192.168.1.210
notepad server/main/xiaozhi-server/data/.config.yaml
```

Set `LLM.DeepSeekLLM.model_name` and `api_key` in that private config. The script
preserves existing config and the persistent signing key on repeat runs. If
switching from a cloud config, ensure `server.ip` is `0.0.0.0` inside the container
and `server.websocket` points to the PC, not localhost. The [local Compose file](server/compose.local.yaml)
publishes only port 8000; configuration/OTA port 8003 stays inside the container.

The local template runs SenseVoice on CPU, DeepSeek through its API, and EdgeTTS
online. This relocates the backend; it does not make LLM/TTS work offline. First
install the speech model if `model.pt` does not already exist:

```powershell
curl.exe -fL --retry 2 https://modelscope.cn/models/iic/SenseVoiceSmall/resolve/master/model.pt -o server/main/xiaozhi-server/models/SenseVoiceSmall/model.pt.download
# Only after curl reports success:
Move-Item server/main/xiaozhi-server/models/SenseVoiceSmall/model.pt.download server/main/xiaozhi-server/models/SenseVoiceSmall/model.pt

docker compose -f server/compose.local.yaml up -d --build
docker compose -f server/compose.local.yaml logs -f --tail 100
```

The [CPU Dockerfile](server/Dockerfile-local) installs Python dependencies in the
container, including CPU PyTorch wheels instead of CUDA. It uses the public ECR
Python mirror by default; `PYTHON_IMAGE` is a build argument if another registry
is needed. No ESP-IDF, Conda or host Python dependencies are required to run the
server. Keep the PC awake and allow inbound TCP 8000 on the Windows private
network if the firewall blocks ESP32 access. Router guest/AP isolation can also
prevent access. Wi-Fi must be 2.4 GHz for the ESP32; the PC may use Ethernet or
5 GHz on the same routed LAN. Reserve the PC address in the router.

Read Device ID / Client ID in Flipper settings, then issue a device credential
using the **same key file** as the running container:

```powershell
python scripts/issue_device_token.py --key-file server/main/xiaozhi-server/data/server.env --device-id DEVICE_MAC --client-id CLIENT_UUID --output server/main/xiaozhi-server/data/device-token.txt
```

The token is written to that private file, never printed. Enter it and
`ws://PC_IP:8000/xiaozhi/v1/` in AI Assistant. An already existing token is valid
only if the local server uses its original signing key and its identity matches;
otherwise issue a new one. Tokens default to 30 days. Do not commit or share
`data/.config.yaml`, `data/server.env` or `device-token.txt`.

```powershell
# Stop / restart later (without rebuilding):
docker compose -f server/compose.local.yaml stop
docker compose -f server/compose.local.yaml up -d
```

## Checks

```text
python -m unittest discover -s tests -v
uv run --with ziglang python scripts/test_native.py
```

This is the initial implemented vertical slice, not a hardware-validated product.
RF/IR transmission tools, generic BLE operations, full Wi-Fi scanning UI, secure
update workflow and power/analog design remain explicitly unimplemented; they
are not advertised as model capabilities. See the status document for limitations.
