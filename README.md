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

The three customized component directories are maintained as ordinary source
directories in this single repository. Unmodified third-party Flipper dependencies
are downloaded separately as ordinary ignored directories, without nested Git
repositories. Upstream licenses remain in downloaded dependencies; revisions,
URLs and SHA256 checksums are recorded in sources.lock.json.
`refer/` was not modified.
See [source revisions](sources.lock.json), [final specification](docs/final-spec.zh-CN.md)
and [implementation/validation status](docs/implementation.md).

## What is downloaded separately

| Content | How to obtain it | Submitted to Git? |
|---|---|---|
| Customized Flipper / ESP32 / server source, scripts and whale artwork | Main repository | Yes |
| 16 upstream Flipper dependencies (including STM32 radio firmware) | `python scripts/fetch_dependencies.py` | No |
| SenseVoice ASR weights/config/tokenizer and Silero VAD ONNX | `python scripts/download_models.py` | No |
| ESP-IDF SDK/toolchains | Install with EIM; see ESP32 section | No |
| IDF managed components | Downloaded by the IDF component manager during configure | No |
| Flipper toolchain | Downloaded by FBT on its first build | No |
| Python libraries / CPU PyTorch | Installed inside the local Docker image | No |
| Firmware packages, logs, caches and private configuration | Generated locally | No |

The download commands are repeatable: completed model files are checksum-checked;
existing dependency sources are kept, including local edits. Incomplete source
directories or mismatched model files cause an error rather than an overwrite.
Downloaded archives live under ignored `build/downloads/`. Use `--check` on either
command to check availability without a network request. Model URLs and SHA256
hashes are in [models.lock.json](models.lock.json); the ASR checkpoint is about
893 MiB. The full models directory is ignored, including small model helpers.

The upstream Java API, Vue/Node console, mobile client, digital-human demos,
resource generator and their deployment/CI files have been removed from this
project. The Python voice service includes its own small admin page, with browser
system fonts and no Java/Node build. Upstream provenance and licenses remain in
this repository; the complete original project is available from its upstream.

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
# First setup (also runs automatically in build_flipper.ps1):
python scripts/fetch_dependencies.py
python scripts/sync_protocol.py
python scripts/export_flipper_assets.py
./scripts/build_flipper.ps1
# Full SD/update package, including original applications and resources:
./scripts/build_flipper.ps1 -Package
```

The build script fetches missing dependencies at their locked revisions, checks
archive hashes, then reapplies our Protobuf extension from protocol/. Existing
dependency directories are preserved. No nested Git repository or submodule
setup is needed. Its toolchain is separate from ESP-IDF.
Output is under `build/release/flipper/`.
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
`data/.config.yaml`, and supply provider settings locally. The selected speech
providers use [these requirements](server/main/xiaozhi-server/requirements.txt).

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

From this project directory, prepare local config, stable device/admin credentials
and downloaded speech models. Replace the IP with the PC LAN IPv4 address:

```powershell
python scripts/setup_local_server.py --host 192.168.1.210
python scripts/download_models.py
docker compose -f server/compose.local.yaml up -d --build
```

Open **http://127.0.0.1:8001**. Copy the admin login token from the private file
`server/main/xiaozhi-server/data/admin-token.txt`. The setup command preserves
existing configuration and both credentials on repeat runs; values are never
printed. The admin token, provider API key and ESP32 device token have different
roles and are not interchangeable.

The page provides model Base URL / model name / API key settings, EdgeTTS voice,
system prompt, tool-calling switch, live devices and device-token issuance.
API keys are never returned to the page. A blank key keeps the active provider's
key, while an explicit clear removes it. Saves use revision checks and atomic
writes, then reconnect the voice service; existing conversations end. The admin
page remains usable when the model is unconfigured or voice models fail to load.
An interface test sends one short model request with a harmless probe tool and
does not operate Flipper hardware. File editing remains available in
`server/main/xiaozhi-server/data/.config.yaml`.

Docker runs one Python process. Device WS is published on port 8000; the admin
page on 8001 is published only to `127.0.0.1`. No Java, Node, SQL database or Redis
is needed. For native Python deployment, admin host defaults to localhost as
well. `server.http_port: 0` disables the old OTA/vision HTTP service; it is separate
from the authenticated admin page. Unavailable vision is not advertised to MCP.
The previous Java management configuration is rejected rather than contacted.

Read Device ID / Client ID in Flipper's AI Assistant settings; select an identity
row to see the full identity split over multiple lines. Issue a device token on
the web page, then enter it and `ws://PC_IP:8000/xiaozhi/v1/` on Flipper. The page
can also fill identities from connected devices. Tokens default to 30 days.
Alternatively, the command-line issuer uses the same server signing key:

```powershell
python scripts/issue_device_token.py --key-file server/main/xiaozhi-server/data/server.env --device-id DEVICE_MAC --client-id CLIENT_UUID --output server/main/xiaozhi-server/data/device-token.txt
```

ASR runs locally on CPU; DeepSeek and EdgeTTS still use online services. First
Docker build downloads Python dependencies, including CPU PyTorch instead of
CUDA. The CPU Dockerfile uses the public ECR Python image and USTC Debian mirror;
`PYTHON_IMAGE` and `DEBIAN_MIRROR` build arguments can select other sources. Keep
the PC awake. Allow TCP 8000 on the Windows private network if required; router
guest/AP isolation can prevent device access. ESP32 needs 2.4 GHz Wi-Fi, while the
PC can use Ethernet or 5 GHz on the same routed LAN. Reserve its IP in the router.

```powershell
docker compose -f server/compose.local.yaml logs -f --tail 100
docker compose -f server/compose.local.yaml stop
docker compose -f server/compose.local.yaml up -d
```

Do not commit/share `data/.config.yaml`, `server.env`, `admin-token.txt` or device
tokens. The current setup keeps them in the ignored data directory.

## How the model controls Flipper

The system prompt describes the character and tool-use rules. ESP32 advertises
actual MCP tool names, descriptions and JSON argument schemas when connected.
The model returns a tool call; the Python server forwards it to ESP32, which
uses the UART Expansion/RPC link to Flipper. Async jobs are awaited for their
actual final result before the assistant reports success. A prompt or skill
file alone cannot add an unimplemented hardware capability.

The full-feature coverage target is documented in [specification section 6](docs/final-spec.zh-CN.md#6-工具范围与应用共存).
There are now 13 Flipper MCP tools:

| Tools | Implemented action |
|---|---|
| `flipper_get_status`, `flipper_system_info` | Companion status, native firmware/hardware and power info |
| `flipper_app_state`, `flipper_list_apps` | PIN/foreground/RPC-app state, main launch names and paginated SD app categories |
| `flipper_app_open`, `flipper_app_exit` | Open main apps or `/ext/apps/...fap`; exit apps supporting this RPC session |
| `flipper_list_files` | Paginated SD directory metadata under `/ext` |
| `flipper_press_button`, `flipper_alert` | One short key input or the built-in Flipper audiovisual alert |
| `flipper_nfc_scan` | Identify NFC protocols, without full authenticated reads or writing |
| `flipper_run_saved` | Bounded IR/Sub-GHz transmission or NFC/LF RFID/iButton emulation from saved SD files |
| `flipper_job_get`, `flipper_job_cancel` | Final task results and cancellation requests |

Update **both** Flipper and ESP32 with the paired release artifacts. Native tools
check the firmware capability handshake and return `firmware_update_required` on
older Flipper firmware. UART discovers at 9600, then requests 460800; negotiation/
RPC startup failure or loss of an established connection latches 115200 until
ESP32 reboot. Reconnection never automatically replays unknown-result tools.
`flipper_get_status` reports `uart_baud` and `uart_fallback`. The Flipper side
already negotiates these rates; the speed change only requires updating ESP32.
All jobs share one UART/RPC session; UI/audio callbacks
only queue work. PIN lock and existing foreground apps are respected. A saved
action loads, runs for a requested 100–5000 ms, then releases/exits and checks that
the foreground slot is free. An unconfirmed stop is a failure, and transmission
never proves the external target reacted. Do not automatically retry unknown
results. App launch/key delivery do not prove an app's semantic action completed.

For example, ask to list `/ext/infrared`, then send the named button from the
selected `.ir` file. Saved files use `/ext/infrared/*.ir`, `/ext/subghz/*.sub`,
`/ext/nfc/*.nfc`, `/ext/lfrfid/*.rfid`, or `/ext/ibutton/*.ibtn`. Apps are not
silently replaced when busy. No actual hardware was used to validate these paths.

The admin page shows the implemented catalog and tools reported by connected
devices. A fixed tool-use policy is appended to the editable persona prompt;
tool calling can be disabled on the prompt/tools page.

Only `server/compose.local.yaml` and `server/Dockerfile-local` are maintained for
deployment (Windows Docker Desktop or Linux). `server/docker-setup.sh` is a small
Linux launcher for that same Compose file. Old full-stack Compose files, Web
Dockerfiles and upstream management workflows are no longer shipped.

## Checks

Run Python tests in an environment with the backend requirements installed, or
use the built Docker image without installing those libraries on the host:

```powershell
docker run --rm --workdir /workspace --mount "type=bind,src=E:\MyWork\DeepFlipper\DeepFlipper,dst=/workspace,readonly" deepflipper-server:local python -m unittest discover -s tests -v
```

Replace the source path above with your project directory. Native codec tests
use Zig separately. Configure/build ESP32 once first to fetch its managed cJSON
component, which the portable native-action workflow tests also compile:

```text
python -m unittest discover -s tests -v
uv run --with ziglang python scripts/test_native.py
```

This is not a hardware-validated product. Live card reads/writes, RF/IR receiving
and learning, generic BLE/GATT, GPIO/ADC/PWM tools, app-specific third-party
actions, full Wi-Fi scanning UI, secure update workflow and power/analog design
remain incomplete. They are not advertised as working model tools. See the
status document for limitations.
