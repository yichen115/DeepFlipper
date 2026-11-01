# Implementation and validation

This document describes the code that exists. The [final specification](final-spec.zh-CN.md)
is the target behavior, not a claim that all milestones have shipped.

## Implemented vertical slice

| Component | Current implementation |
|---|---|
| Flipper system | Resident DeepFlipper state and worker; does not occupy the Loader application slot |
| Mascot | Desktop animation hook, native 64x64 character on the left, firmware fallback, fixed right column: clock/battery, bold activity plus contextual hint, aligned audio controls and lock/attention icons; cached controls hidden offline |
| Serial | One Expansion session, 9600 discovery → 460800 preferred / 115200 latched fallback; multiplexed project/native RPC, streamed/async replies and continuous heartbeat |
| UI/settings | English status/IP/setup-password/help and audio shortcuts; no long-text credentials entry; live updates preserve selection and refresh the setup address |
| Passport | Whale-girl bust portraits with collar/shoulders for happy/normal/angry moods, without a separate whale emblem; original nickname, level and XP mechanics retained |
| Configuration | Authenticated ESP32 AP/LAN web setup, versioned NVS profile, temporary Wi-Fi trials, identity/config file exchange, periodic UART status and bounded shortcut commands |
| Tools | 20 MCP tools: adds live NFC read, IR learning, OOK RAW capture, SD file read/space, clock and GPIO; app lifecycle, short/long keys, saved actions and query/cancel |
| Application coexistence | Loader admission cancels assistant NFC, frees the mascot worker stack, and yields UART RPC heap for local apps; RPC-origin app launches preserve their command transport; exit/failure restores the worker and local-app UART |
| ESP32 source | Dedicated S3 board, reference I2S audio, GPIO48 WS2812, local button, UART RPC/MCP bridge |
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
- Full encrypted/application-specific card reads/writes, generic FSK decoding,
  BLE/HID, ADC/PWM and arbitrary third-party app semantic actions remain unimplemented.
  Opening an app does not make its internal features into working AI tools.
- Wi-Fi supports ordinary 2.4 GHz personal/open networks, scan/manual SSID and
  AP/LAN web configuration. Enterprise authentication, static IP and device OTA
  management are not implemented. Higher-rate UART, AP/STA channel changes,
  reconnect/power-loss persistence and actual audio still require hardware tests.
- The server probe checks an authenticated WebSocket hello without capturing
  audio. It does not certify provider API configuration, ASR/TTS or speaker/mic
  operation; those have separate playback/conversation actions.
- The physical module switch and power-off UART isolation require hardware.
  The AI-session setting stops communication; it is not a physical power switch.
- The standard `deepflipper-s3` build drives one GRB WS2812 on GPIO48 using RMT
  and the same status/brightness controls. Three-channel PWM RGB has been removed.
  The actual LED pin, color order and low-brightness behavior require board testing.
- Firmware update recovery, low-battery policy and global hardware resource
  compatibility beyond the initial NFC path need additional implementation/tests.
- Normal mascot animation is replaced; existing blocking warnings and special
  legacy screens are preserved. Desktop layout and actual LCD ghosting need QA.
- Fixed English UI is implemented. SSID/password input uses the UTF-8 browser page;
  Flipper shows short English network state rather than rendering dynamic SSIDs.

## Validation performed

The local-app memory fix is covered by host tests of the production C functions:
300 repeated RPC allocations/teardowns, UART versus USB/BLE/ordinary-expansion
profiles, notification after full free, cancellation while waiting for the Loader
barrier, and repeated local app suspend/exit/failed-launch cycles. These establish
software lifetime/order behavior, not measured heap margins or successful physical
NFC reads. Launch diagnostics log free heap and largest free block after yielding
UART; actual cards and the device heap must still be checked on hardware.

- The untouched Unleashed base compiled with the bundled Flipper toolchain.
- The modified firmware and the full `updater_package DEBUG=0` target build.
  Packaging checks the firmware/radio memory boundary; no dangerous override is
  passed. Outputs are copied to `build/release/flipper/` by the build script.
  The current validated firmware/radio gap is 6560 bytes; future core additions
  must continue to pass this check. Matched SD resources include the character
  frame file, not just a separately copied preview asset.
- Initial normal size optimization exceeded the C2 boundary. Release builds now
  use size optimization/LTO for higher-level code; Furi, Cortex-M FreeRTOS ports
  and startup/interrupt code remain non-LTO because of naked assembly literal
  pools. External FAP compilation stays non-LTO. The whole matched resource/app
  package is rebuilt. Existing upstream startup-signature LTO warnings remain
  visible; a successful build is not a runtime certification.
- 40 project Python tests pass on Windows and in the Linux Docker image: identity binding, allowlist behavior, expiry/
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

## ESP32 device setup page

The dedicated board owns `device_setup.*`, `portal_config.h` and embedded
`web/deepflipper_setup.*` assets. It uses the IDF Wi-Fi/HTTP server directly;
managed upstream components are not edited. The board inherits the common Board
interface and forwards network events to Application. Both AP and STA access use
one authenticated HTTP service. Wi-Fi callbacks only record state; the setup task
owns scans, retries, temporary credentials and server probes. Application/audio
mutations are scheduled on the application task. A default no-op Board callback
reports real voice WS phases without importing board types into the protocol.

The old profile and SSID list migrate once when upgrading the app without
wiping NVS. The full merged BIN includes padding over the NVS region and resets
configuration/identity; existing installations with matching partitions and
unchanged ota_0 boot selection can update only xiaozhi.bin at 0x20000. New Wi-Fi credentials remain a trial
until matching SSID/IP confirmation; failure keeps the stored profile and recovery
AP. Profile edits use one NVS JSON value with revision checks. The web API returns
public fields and secret-presence flags; API responses are sent after releasing
configuration locks so a slow browser cannot stall UART state reporting. Session
cookies remain alive until response headers have actually been transmitted.
Login uses a salted PSA SHA-256 digest, rate limiting, expiring HttpOnly/SameSite
sessions and CSRF/Origin checks. The initial setup password is sent only over the
trusted companion UART for local viewing, never through `flipper_get_status`.
Holding the talk button 5 seconds recovers the web login/AP without clearing the
profile or identity. Character state and LED colors use known network/authentication
failures instead of displaying ready after a failed connection; LED pulses continue
during long native operations, with refresh bounded to 50 Hz. AP, authentication probes and active voice keep Wi-Fi awake; the normal low-power
request restores modem sleep after the voice connection closes and AP is off.
Driver power changes use a separate lock so Wi-Fi event callbacks can still take
the configuration lock. Physical current draw and analog power behavior remain
to measure.

The Python admin imports device identities and exports device-bound connection
JSON via an authenticated endpoint. ESP32 validates format/version and both IDs
before merging URL/token. Portable C++ tests cover identity binding, invalid
fields, secret preservation/explicit clearing, confirmed Wi-Fi changes, bounded
JSON parsing, session/CSRF expiry and session replacement. Python tests cover
export auth, token binding, redaction, invalid URLs and revocation. Browser QA
exercised login, scan selection, Wi-Fi submit/status, identity download, real
Python config export, config import and volume save. The ESP32 HTTP/network side
in that browser QA used a simulated device; no physical link was tested.

## Full setup workflow review

Wi-Fi trials now carry request IDs and explicit queued/connecting/saved/failed
results with commit revisions. Trials only confirm a matching, timely IP after
the old association has disconnected. Restoring the old network retains the
failed trial result and leaves recovery AP available. Network changes allow a
short response grace period before disconnecting, and unknown results are queried
rather than replayed. Failed scans return empty results instead of uninitialized
records. Web status separates current network state from requested-change results.

The frontend tracks drafts and revisions per form, sends only changed settings,
and does not clear a pasted token when volume or Wi-Fi is saved. Edits made while
a request is in flight survive its acknowledgement. Known local commits can
advance unrelated draft revisions; unknown external changes require explicit
refresh, preserving the draft. The page links to the assigned LAN IP, distinguishes
manual AP from its shutdown countdown, shows recording state and offers next-step
instructions. Authentication tests replace their pending message with a final
result. Server-side identity/URL edits invalidate stale downloadable profiles;
exports include issuance/expiry metadata. App-only/full-image instructions ship
in the ESP32 release directory.

Startup/activation ownership is preserved during AI-session suspension and
configuration reloads. A direct-voice idle device without a protocol retries
initialization when network connects. Reloads report completion so the page cannot
start a conversation before the new protocol is ready. Session suspension and
reload clear queued playback, while microphone disabling ends current capture.
WebSocket teardown joins the receive task before deleting its event group;
reconnections clear old hello/session state and reject incomplete direct-server
hello messages. Explicit authentication rejection no longer waits for a timeout.
Speaker/talk prerequisites explain muted, zero-volume, disabled-microphone,
unconfigured, disconnected and busy states. These are compiled/simulated tests;
real audio, radio and power behavior still require the board.

## UART offline diagnosis

The device reported UART offline while its AP was running. Review found a concrete
session bug: the Flipper hardware-random uint32 session could exceed INT32_MAX,
while ESP32 read cJSON.valueint, which saturates. The initial hello could succeed
but all subsequent session-bound requests would fail. Session/command/remote-job
IDs now use validated full-width numeric decoding. Tests exercise UINT32_MAX and
both halves of the signed boundary with JSON echoing.

Discovery now sends the same UART 0xAA presence byte as upstream expansion_test;
the previous 10 ms GPIO-low break could trigger a peer framing-error exit and also
unnecessarily rerouted the TX pin. Frame encoding is tested against Flipper's real
C parser/encoder (compiled as C, preserving its empty-struct layout). Boot attempts
log rate-limited handshake stage, RX byte/error counts and ACK status. Authenticated
device status also exposes these diagnostics, never credentials. The UART0 console
is independent from UART1 IO17/18; hardware notes now distinguish both pin groups.
The user's qFlipper showed the custom 30 Oct 2026 build and AI ON/UART offline;
USB was occupied by qFlipper and COM42 was denied while its console tool was open.
No corrected firmware was flashed or a successful physical link claimed here.

The subsequent single-RX tap captured repeated hello requests and session IDs
272386472..474. The reply contained an unquoted/duplicated `native_rpc` suffix.
Running the actual upstream Frozen formatter with session 272386472 reproduced
exactly `{"ok":true,"v":1,"session":272386472,native_rpc:1},"native_rpc":1}`.
Frozen copied the remaining JSON format into a temporary printf format instead
of terminating it after the current conversion. This also allowed later printf
conversions to read arguments belonging to Frozen-specific specifiers. The local
Flipper copy now bounds and terminates the individual conversion before printf;
reference sources remain untouched. Native tests compile the actual formatter
and strictly parse hello/commands/status/job replies with ESP32's cJSON, including
sessions above INT32_MAX and truncated output buffers. This correction requires
a new Flipper update package; it does not require another ESP32 rebuild. Physical
link stability remains to be checked after installation.

The next capture contained one valid hello (session 2595840768), followed by
hundreds of accepted UI/poll requests before going offline. Near the tail a
heartbeat byte appeared before a data frame, with its matching byte after the
frame. The capture is a lossy text export (high bytes replaced with '?'), so it
cannot establish every checksum or electrical error. Inspection nevertheless
found and host-reproduced a concrete TX race: the Expansion worker sends
heartbeat/status on its thread while the RPC thread sends data, and the UART
driver and frame encoder had no shared lock. The encoder writes the frame body
and checksum separately. A per-worker mutex now covers both writes and remains
allocated until the RPC/worker threads have stopped. The same forced-preemption
test fails on unmodified reference code and passes 6000 mixed frames with the fix.

ESP32 keeps the standard discovery/baud/RPC/hello sequence. Steady-state command
polling is limited to 200 ms; unchanged UI refreshes every 2 s, with state/mouth/
emotion changes still eligible every 50 ms. Full public module state is checked
once a second and sent on change or every 5 s. An explicit receive pump preserves
the independent 100 ms heartbeat when updates are quiet. Fatal receive/ACK/RPC
errors stop transport traffic immediately, retain a credential-free disconnect
reason in logs/web diagnostics and return to discovery instead of continuing
with session zero. Ten-minute simulated scheduling tests verify idle limits and
immediate changes; these are not a ten-minute hardware endurance test.

Settings help now uses a bounded 124x50 scroll/wrap element below the title.
Network status, setup credentials and identity can be read with Up/Down instead
of clipping the fifth/sixth line or long state names. Unchanged snapshots do not
rebuild the help widget, preserving its current scroll position.

## Manual conversation controls

With AI enabled, desktop short OK queues one `talk:toggle` module command;
long OK opens the system menu, including when the RTC debug flag is enabled.
AI-off behavior and dummy-mode shortcuts remain unchanged. The mascot hints
now say `OK: talk` / `OK: send`. ESP32 validates start prerequisites and handles
stop independently of start readiness; UART, GPIO0/BOOT click and HTTP use one
scheduled main-task path. Command IDs continue to suppress repeated UART polls.

The device web page puts voice control immediately after status, above network
settings. The same primary button changes from start to send while listening;
the start action being busy must not disable sending. It posts `/api/stop-talk`,
then refreshes status, and does not automatically replay an uncertain request.
Host tests cover toggle/start/stop eligibility and idle/connecting/listening
button states. Physical recording/ASR/LLM/TTS completion remains unverified.

On 31 Oct the local service was ready and later showed this device with its MCP
tools registered. The PC WLAN had changed to 192.168.2.8; the advertised URL was
updated from 192.168.1.210 to `ws://192.168.2.8:8000/xiaozhi/v1/`. TCP 8000 was
reachable on the WLAN address and a LocalSubnet inbound firewall rule was added.
This does not change a URL already saved on ESP32; device configuration must use
the current LAN address. The user confirmed audible speaker-test output and a
listening state, which do not by themselves prove microphone capture/recognition.

## Conversation history and voice/link diagnostics

The admin history tab reads an authenticated, paginated device-filtered API.
A bounded background queue writes user/assistant dialogue and processing events
to local SQLite WAL; the latest 10000 events survive reconnect/restart. System
prompts, temporary examples, raw tool results/arguments and audio are excluded.
Record text renders via DOM textContent. Reconnection creates a new session ID
without erasing older text; failed TTS records an error independently of a model
reply. Tests cover authentication, device filtering, paging, concurrent writes,
retention, restart, template/credential exclusion and untrusted HTML as text.

The user's voice session did reach listen:start and listen:stop in server logs.
An EdgeTTS network failure to speech.platform.bing.com was independently reproduced
with a bounded synthesis probe (timeout). TCP alone succeeded, so TCP reachability
does not establish a working speech WebSocket. Windows' configured loopback proxy
also failed a bounded speech-HTTP probe; no proxy settings were changed. EdgeTTS
now uses bounded connection/receive timeouts and records synthesis errors. This
network condition remains an external limitation; no successful reply playback
is claimed and it does not establish the reason for the user's UART No link.

Expansion RX buffering is now four frames and its worker uses normal priority;
overflow explicitly fails the worker instead of silently losing a byte. ESP32
diagnostics include reset cause, uptime, heap minimum, UART task stack headroom
and maximum gap between receive-pump calls. One disconnect reason per ten minutes
is retained in NVS, so power cycling does not erase all diagnostic evidence. The
device was switched off for charging before a physical failure reason could be
read; power integrity, real UART stability and the full audio round trip still
require hardware verification.

## Local speech synthesis

Local CPU TTS now uses the pinned sherpa-onnx MeloTTS Chinese/English female
model. The 167006755-byte archive SHA256 is recorded in models.lock.json;
download/extraction validates checksums and paths, retains licenses, and never
commits weights. The existing runtime is reused without a separate service/GPU.
The model engine is cached and serializes inference; PCM is encoded into WAV
then resampled/encoded by the existing device audio path. The admin page selects
LocalTTS/EdgeTTS and offers an authenticated preview without mutating settings.
Current private configuration was switched to LocalTTS, preserving API keys.

In a network-disabled container the real model loaded in 2.25 seconds and
generated 7.12 seconds of audio in 1.98 seconds. The live authenticated preview
returned 2.86 seconds of WAV in 1.0 second, and the voice runtime reported ready.
These are CPU measurements on this machine, not speaker playback evidence.
TTS attempts now have the configured timeout and bounded retries; a failed turn
queues exactly one standard tts:stop and skips remaining audio for that turn.
Existing ESP32 handles this terminal message without a firmware change. Regression
tests cover timeout recovery, partial files, callback continuation and tts:stop.

## Audio level and native system-info correction

The user reported low/noisy voice and a native system-info failure at desktop_state.
The server log contained rpc_status=1 at that stage. Actual Flipper
rpc_desktop_on_is_locked_request returns an Empty reply: status 0 means locked,
status 1 means unlocked. The ESP32 adapter had incorrectly expected Desktop.Status
tag 70 (an unsolicited subscription event), and its generic error check rejected
the normal unlocked result. The adapter now decodes the request's legacy boolean
semantics, including lock rechecks during saved actions. Read-only system/power
information bypasses the unnecessary desktop admission check entirely. Native
tests compile the actual Flipper lock handler, rather than inventing its replies.
The model tool policy also forbids diagnosing version mismatch from generic RPC
or desktop errors alone. This correction needs an ESP32 application update; no
Flipper-side protocol change was made in this correction.

Measured local TTS WAV examples were around -30 dBFS RMS. Generated speech now
targets -20 dBFS RMS, limits gain to 16 dB and retains 1 dB peak headroom; near-
silence is not amplified. The live preview measured -20.0 dBFS RMS, -6.6 dBFS peak
and zero clipped samples. Device audio initially prebuffers four 60 ms packets
instead of two, with queue-order tests. On the DeepFlipper board alone, speaker
I2S now outputs identical 32-bit left/right slots, avoiding L/R-average attenuation
and channel-strap dependence; its PCM packing is tested at signed limits, half
volume and mute. A fixed scratch buffer replaces per-frame TX allocations. Mic
format and pins are unchanged. Electrical noise, supply limits and the user's
amplifier are still unmeasured; these changes are not a physical noise diagnosis.

## ESP32 browser audio preview

The setup page now prepares a shared 24 kHz/16-bit mono buffer for its computer
player and the device. It supports a fixed embedded speech WAV, a fade-in/out
440 Hz tone, and browser-decodable user files up to 30 s / 10 MB. Format/metadata
checks precede conversion; the source level is preserved. Ordered 64 KiB binary
POSTs use the existing session/CSRF/origin checks and a random upload ID. The
device validates sample count, offsets, chunk length and full completion before
playing. Total PCM is capped at 1.44 MB in PSRAM, not NVS/flash. Idle preview
workers block on notifications; abandoned uploads expire after 15 s.

Playback feeds the existing bounded AudioService PCM/output queue from a separate
task, preserving the board codec's volume/mute and mouth-level hooks. It closes
the idle voice channel first and waits for its main-task completion. A guarded
preparation lease prevents a cancelled/old preview from suspending a later voice
recording. Tests compile the real queue and preparation functions, and cover
bounded ownership, chunk/commit validation, expiry, cancellation and stale browser
responses. Browser QA exercised speech decoding, tone preparation, WAV selection,
full mock upload, completion and stop; build/audio-preview-ui.png uses simulated
device status and is not evidence of physical speaker output. ESP32 hardware
playback remains to be verified after flashing; server/Flipper RPC is unchanged.

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

## Current expansion, context and power changes

Twenty MCP tools now include the capture adapter, ranged SD reads, capacity, RTC
and GPIO modes/levels. The capture FAP is built and checked against the firmware
SDK, then bundled into SD resources as apps/Tools/deepflipper_tools.fap. It runs
under the shared RPC lifecycle, confirms start separately from actual result,
writes a per-session/job result file after releasing hardware, and is exited
with Loader release verification. IR saves decoded or assumed-38kHz RAW signals;
Sub-GHz captures internal-radio OOK650 RAW after RSSI crosses -75 dBm, with at most
4096 timings. NFC reads public Type2 pages and Classic with stock cached keys;
without suitable keys/family support it returns identification or failure rather
than claiming full read. Hardware acquisition and replay remain unverified.

RPC callbacks detach before publishing stop, so the SD app never dereferences a
freed session or frees context still owned by a callback. Host tests compile the
actual callback and check exit/session-close ordering. Native action tests cover
result identity, capture cleanup, file bounds, reserved GPIO rejection and long
input. Existing transmit/emulate paths remain bounded and report target_state
verification separately from local completion.

Conversation restoration now loads only complete user/assistant pairs from the
same device and client identity, at most 6 turns by default, 6000 characters and
24 hours. Admin settings allow 0–12 turns; restored text is not re-recorded. No
raw tool calls are replayed. SQLite history remains separate from live context.

Bluetooth was already disabled. PM is enabled for the S3 variant, with 80–240 MHz
CPU scaling and no automatic light sleep. Idle direct voice returns Wi-Fi to
modem sleep even while the WebSocket stays open. Manual ESP32 standby stops AP,
STA and voice but keeps UART commands; desktop OK or BOOT click wakes Wi-Fi.
The active Flipper Expansion service still blocks deep sleep. No battery/current
measurements or headset support are claimed; S3 lacks Classic HFP/A2DP.
