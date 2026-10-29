# Implementation and validation

This document describes the code that exists. The [final specification](final-spec.zh-CN.md)
is the target behavior, not a claim that all milestones have shipped.

## Implemented vertical slice

| Component | Current implementation |
|---|---|
| Flipper system | Resident DeepFlipper state and worker; does not occupy the Loader application slot |
| Mascot | Desktop animation hook, native 64x64 character on the left, firmware fallback, fixed right column: clock/battery, bold activity plus contextual hint, aligned audio controls and lock/attention icons; cached controls hidden offline |
| Serial | One Expansion session, 9600 discovery → 460800 preferred / 115200 latched fallback; multiplexed project/native RPC, streamed/async replies and continuous heartbeat |
| UI/settings | English AI Assistant settings with in-place label refresh preserving selection/scroll, offline/busy feedback, phone hotspot instructions, raw ASCII/symbol page and masked secrets |
| Passport | Whale-girl bust portraits with collar/shoulders for happy/normal/angry moods, without a separate whale emblem; original nickname, level and XP mechanics retained |
| Configuration | One pending module command with transaction ID, bounded deadline and final module reply |
| Tools | 13 MCP tools: status/info, NFC protocol scan, directory/apps, app lifecycle, short keys/alert, five saved-file action kinds, query/cancel |
| Application coexistence | Loader admission cancels assistant NFC and waits for release before normal app start |
| ESP32 source | Dedicated S3 board, reference I2S audio, PWM RGB, local button, UART RPC/MCP bridge |
| Voice | Reuses XiaoZhi ASR/LLM/TTS transport; direct WS/WSS configuration bypasses managed OTA activation |
| Animation linkage | State/emotion events and actual output PCM levels; no transcript/caption forwarding |
| Module persistence | Single NVS profile plus restored driver settings; mute/zero-volume startup preserved |
| Backend | One Python process for WS voice and authenticated local admin UI; masked API settings, prompts/tool policy, device-token issuer, atomic config updates and voice reload |
| Tool results | Async server job polling; timeout/cancel/busy reported as failure, not accepted-as-success |

## Explicitly incomplete

- No hardware has been flashed, and no real audio/RPC/NFC end-to-end session has
  been measured. GPIO and analog circuitry remain a reference, not a verified PCB.
- ESP32 target compilation has passed through the user's EIM-managed `v6.1`
  environment. The SDK is `D:\esp\v6.1\esp-idf`, tools are in
  `D:\Espressif\tools`, and the matching Python environment is selected by EIM.
  Actual board wiring, voice operation and power consumption remain untested.
- Live card-specific reads/writes, RF/IR reception/learning, generic BLE/GATT,
  GPIO/ADC/PWM tools and third-party app semantic actions remain unimplemented.
  Opening an app does not make its internal features into working AI tools.
- Wi-Fi currently supports manual SSID/password and the inherited phone setup
  portal. A paginated on-device scan list, explicit forget/reset flows, config
  revision conflict UI and detailed connection diagnostics remain to implement.
- The server-config action reapplies the transport configuration; it does not
  certify a successful TLS/authentication/voice handshake. Authentication is
  actually checked when the voice socket opens.
- The physical module switch and power-off UART isolation require hardware.
  The AI-session setting stops communication; it is not a physical power switch.
- The default RGB reference is PWM/common-cathode; the suggested GL5050 part's
  electrical details have not been established. LED sequencing still needs
  hardware calibration; audio/LED thresholds are initial reference values.
- Firmware update recovery, low-battery policy and global hardware resource
  compatibility beyond the initial NFC path need additional implementation/tests.
- Normal mascot animation is replaced; existing blocking warnings and special
  legacy screens are preserved. Desktop layout and actual LCD ghosting need QA.
- Fixed English UI is implemented. Non-ASCII SSIDs and arbitrary Unicode input
  need a defined display/input fallback; the phone portal is the current path.

## Validation performed

- The untouched Unleashed base compiled with the bundled Flipper toolchain.
- The modified firmware and the full `updater_package DEBUG=0` target build.
  Packaging checks the firmware/radio memory boundary; no dangerous override is
  passed. Outputs are copied to `build/release/flipper/` by the build script.
  The current validated firmware/radio gap is 6920 bytes; future core additions
  must continue to pass this check. Matched SD resources include the character
  frame file, not just a separately copied preview asset.
- Initial normal size optimization exceeded the C2 boundary. Release builds now
  use size optimization/LTO for higher-level code; Furi, Cortex-M FreeRTOS ports
  and startup/interrupt code remain non-LTO because of naked assembly literal
  pools. External FAP compilation stays non-LTO. The whole matched resource/app
  package is rebuilt. Existing upstream startup-signature LTO warnings remain
  visible; a successful build is not a runtime certification.
- 38 project Python tests pass on Windows and in the Linux Docker image: identity binding, allowlist behavior, expiry/
  future credentials, revocation, final job results, timeout cancellation and
  the real configuration route refusing anonymous token issuance before reading
  an unauthenticated request body, plus stale-build-result rejection and local
  setup preserving existing credentials/configuration, plus real WebSocket
  admission handling case-insensitive ESP32 identity headers without bypassing
  device token verification. Dependency download tests cover safe archive
  extraction, portable file links and preserving incomplete/local sources. MCP
  capability tests preserve tool negotiation while withholding disabled vision
  capabilities and token generation. Executor tests cover native tools waiting
  for final job results and job-management calls avoiding recursive polling.
- The `deepflipper-s3` reference variant builds and merges under IDF v6.1/GCC
  15.2. It defaults to manual start/stop, no wake word and no AFE processing.
  The EIM wrapper validates a per-build nonce and merged-image hash, since the
  EIM outer process may report success even when the child compiler fails.
- Native C++ RPC codec test passes golden bytes, maximum length, malformed
  varints, truncated messages and rejected RPC command status. WS/WSS URL
  validation covers LAN IPv4/DNS addresses and rejects malformed authorities,
  credentials in URLs, invalid ports and control characters. The current
  transport URI parser does not support IPv6 literals.
- Portable native-action tests use a simulated protobuf transport to cover all
  five saved-file kinds, asynchronous startup delay/failure, busy/PIN lock,
  timeout/cancel cleanup, failed stop/reset, app errors, paginated directories
  and draining oversized info streams with bounded output. Codec tests also
  cover fragmented/streamed native replies, async state and batched key input.
- Expansion rate tests cover little-endian 460800/115200 payloads, absence of
  a peer preserving the first fast attempt, rejected negotiation/start/hello,
  established-link loss and fallback staying latched until reboot. The UART
  task restarts discovery after failure with a bounded backoff, waits for a
  valid heartbeat, and switches speed only after the old-rate ACK. Existing
  Flipper rate negotiation needs no firmware change. This is simulated-peer
  testing, not a measured 460800 link on real wiring.
- Eighteen selected XiaoZhi board/menu/configuration tests pass. An additional
  82-test build-script run had five Windows temporary-directory cleanup errors;
  it is not presented as passing.
- A broader inherited 140-test host run was attempted: 12 environment-dependent
  errors remain on Windows (temporary-directory working-directory cleanup,
  CI Bash/Git execution and unavailable host test compiler), with 19 skipped.
  This run is not presented as passing or as an ESP32 target build.
- Sprite data, manifests and C data exports are checked independently. Tests
  and output details are recorded in `build/validation.json` when exported.

## Local backend verification

The local CPU Docker image has been built on Windows Docker Desktop. `pip check`
reports no broken requirements. The downloaded SenseVoiceSmall checkpoint loads
with all model keys matched. An isolated container smoke check initializes the
actual server and speech models, rejects a WS client missing its token, and
returns the voice-protocol hello to a valid device-bound client using ESP32-style
`Device-Id` / `Client-Id` headers. The check uses temporary credentials and no
real provider API calls. The local deployment config and signing key have been
prepared, but real voice chat still requires the user's DeepSeek model/API key,
ESP32 connectivity and audio hardware. It has not been certified end to end.

The container does not publish configuration/OTA port 8003. WS is an explicit
LAN option, with mandatory device authentication; WSS certificate verification
remains intact. See the project README for local startup and device enrollment.

## Repository layout

Customized Flipper, ESP32 and server sources are ordinary directories in the
main repository. Their nested Git metadata has been detached; the user's main
Git history is preserved. Unmodified third-party Flipper dependencies are ignored
and restored from pinned source archives using `scripts/fetch_dependencies.py`.
`sources.lock.json` records revisions, URLs, checksums and required files. Existing
sources are never overwritten. The local Protobuf changes are reapplied from the
tracked `protocol/` sources by the normal build script after dependencies exist.

All 16 dependency archives have been downloaded and restored into a fresh test
tree without nested Git metadata, including nested dependency directories and
portable nanopb header links. ASR/VAD weights and helpers, optional digital-human
resources and web wake-word/model packs are excluded from Git. The required CPU
voice model assets are separately restored by `scripts/download_models.py` from
`models.lock.json`; the runtime model directory stays mounted into Docker.

Unused upstream server documentation/screenshots and the web asset generator's
bundled ESP32 font files have been removed from the source tree. Local voice
deployment instructions are maintained in the project README, and the compact
server README links to it. The Python voice runtime, deployment configuration,
licenses and Flipper menu fonts are preserved. The removed files remain in an
ignored build directory because automatic safety checks rejected recursive deletion.

Component Git metadata remains in an ignored build directory because automatic
safety checks rejected recursive metadata deletion. It is not a dependency or
part of the source distribution.

## Python-only server and admin verification

Java API, Vue/Node console, mobile app, digital-human demos, web deployment files,
full-stack Compose templates and upstream CI were moved out of the source tree.
Only the Python voice service and its native HTML/CSS/JS admin page remain. The
page has no Node build, external JavaScript dependency or bundled font files.
The canonical deployment is compose.local.yaml with Dockerfile-local.

The admin page runs in the same process as voice, on port 8001 published only to
localhost. A separate persistent admin login token is generated by setup; login
uses an HttpOnly/SameSite session and writes require CSRF checks. Saved API keys
are never returned, blank inputs retain the current key, and clearing is explicit.
Settings are written atomically with revision conflict checks. Device tokens use
the existing device-bound signing key. Missing API settings or speech models do
not terminate the admin page. Save/apply reconnects voice clients. Local-only
configuration rejects the removed Java-manager mode.

The editable persona prompt is supplemented with a fixed tool-use policy. Actual
hardware capabilities come from MCP tool names/descriptions/JSON schemas reported
by ESP32. The current catalog includes status, NFC protocol scan, job query and
cancellation. The admin can enable/disable model tool calling; it does not add RF,
BLE or card-write implementations. Flipper identity rows now open a detail view
showing the full Client ID split across lines for device enrollment.

35 Python regression tests cover admin authentication, CSRF/origin checks,
secret redaction, key retention/clear, atomic saves/conflicts, a local mock model
probe, device-bound token issuance, local-only config, and existing auth/jobs/
build/download paths. The real container smoke check verifies WS auth/hello and
MCP tools, admin auth/redaction and token issuance, live settings reload, and
continued admin availability after clearing the API key. Browser login and the
model, prompt/tools and device tabs were checked in an isolated test instance;
browser screenshot capture was unavailable. No real provider API was called by
these tests. Real conversation and physical NFC/audio still need hardware.

## Flipper feature coverage audit

The source manifests and native Protobuf schemas were reviewed across the main
menu and system apps. There are 279 declared App entries (including services,
plugins, examples and debug tools), while the current SD resource package contains
27 FAPs. These are not interchangeable counts of user-visible capabilities.
Native RPC families include System, Storage, App, Gui, Gpio, Desktop, Property,
Network and Gps, plus the project extension. NFC, LF RFID, iButton, Infrared and
Sub-GHz have native RPC application paths. In particular, loading a saved NFC/
RFID/iButton file can start emulation; it is not a general live read-card command.

The final specification targets all primary firmware capability groups. The
ESP32 wire decoder now handles native System/Storage/App/Desktop/Gui replies,
streaming `has_next`, fragmented messages and unsolicited app-state messages on
the same UART. Matching Flipper `hello` advertises `native_rpc:1`; older firmware
is rejected for native jobs. AppState response headers explicitly use command ID
0. The Expansion heartbeat stays active during native waits, and the companion
watchdog allows 10 seconds between status/UI refreshes.

There are 13 registered tools; see the README catalog. The portable
`native_actions.h` engine owns native workflows, with the UART bridge supplying
transport, clock, cancellation and async app state. It waits for app readiness,
loads saved files, sends IR/Sub-GHz or emulates NFC/RFID/iButton for a requested
100–5000 ms, then stops/exits and confirms the Loader slot is free. Busy apps
are not replaced. PIN lock allows info/state queries but denies mutations and
directory browsing. Failed/unknown stops reset the RPC transport; they never
report hardware success. `target_state_verified:false` distinguishes a local
send from a verified target reaction. Short key input is a single batched
press/short/release sequence, and its result only claims event delivery.

MCP callbacks only submit jobs; the UART task executes them. Results persist
until consumed/reused or the 30-second retention expires. Server polling handles
all queued `flipper_*` actions, with a native wait budget of up to 30 seconds;
job get/cancel themselves do not recursively poll. The admin catalog, device
schemas and fixed model policy describe the implemented boundary. App discovery
supports main launch names and paginated `/ext/apps` categories; launching a FAP
does not implement its internal semantic actions. Generic BLE central/GATT and
GPIO adapters remain future work and must preserve the assistant UART/power link.

Both target builds pass and paired firmware must be installed together. No
physical hardware action has been verified. Larger future adapters should use
existing apps or SD FAPs while preserving the remaining internal-flash/radio
boundary. See section 6 of the final specification for full coverage targets.

## Development entry points

- `firmware/flipper/applications/services/deepflipper/`
- `firmware/flipper/applications/services/rpc/rpc_deepflipper.c`
- `firmware/flipper/applications/settings/deepflipper_settings/`
- `firmware/esp32/main/boards/deepflipper-s3/`
- `server/main/xiaozhi-server/core/device_auth.py`
- `server/main/xiaozhi-server/core/providers/tools/device_mcp/deepflipper_jobs.py`
- `protocol/README.md`, `scripts/build_flipper.ps1`, `scripts/issue_device_token.py`

The next hardware milestone is a UART status round trip, then real voice output,
then a bounded NFC scan while the mascot remains on the desktop. Do not use an
unverified reference pin map as permission to connect arbitrary existing boards.
