# Implementation and validation

This document describes the code that exists. The [final specification](final-spec.zh-CN.md)
is the target behavior, not a claim that all milestones have shipped.

## Implemented vertical slice

| Component | Current implementation |
|---|---|
| Flipper system | Resident DeepFlipper state and worker; does not occupy the Loader application slot |
| Mascot | Desktop animation hook, native 64x64 character on the left, firmware fallback, fixed right column: clock/battery, bold activity plus contextual hint, aligned audio controls and lock/attention icons; cached controls hidden offline |
| Serial | Existing Expansion framing carrying a bounded project-specific system RPC envelope |
| UI/settings | English AI Assistant settings with in-place label refresh preserving selection/scroll, offline/busy feedback, phone hotspot instructions, raw ASCII/symbol page and masked secrets |
| Passport | Whale-girl bust portraits with collar/shoulders for happy/normal/angry moods, without a separate whale emblem; original nickname, level and XP mechanics retained |
| Configuration | One pending module command with transaction ID, bounded deadline and final module reply |
| Tools | Status query and NFC protocol scan, up to 10 seconds, one NFC owner, query/cancel |
| Application coexistence | Loader admission cancels assistant NFC and waits for release before normal app start |
| ESP32 source | Dedicated S3 board, reference I2S audio, PWM RGB, local button, UART RPC/MCP bridge |
| Voice | Reuses XiaoZhi ASR/LLM/TTS transport; direct WS/WSS configuration bypasses managed OTA activation |
| Animation linkage | State/emotion events and actual output PCM levels; no transcript/caption forwarding |
| Module persistence | Single NVS profile plus restored driver settings; mute/zero-volume startup preserved |
| Backend | Device-bound token verification, enrollment/revocation checks, protected configuration route; local CPU Docker Compose and persistent-key setup |
| Tool results | Async server job polling; timeout/cancel/busy reported as failure, not accepted-as-success |

## Explicitly incomplete

- No hardware has been flashed, and no real audio/RPC/NFC end-to-end session has
  been measured. GPIO and analog circuitry remain a reference, not a verified PCB.
- ESP32 target compilation has passed through the user's EIM-managed `v6.1`
  environment. The SDK is `D:\esp\v6.1\esp-idf`, tools are in
  `D:\Espressif\tools`, and the matching Python environment is selected by EIM.
  Actual board wiring, voice operation and power consumption remain untested.
- Current model tools are status and NFC protocol detection. RF/IR transmission,
  arbitrary BLE operations and card-specific reads/writes are not registered.
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
  The current validated firmware/radio gap is 7136 bytes; future core additions
  must continue to pass this check. Matched SD resources include the character
  frame file, not just a separately copied preview asset.
- Initial normal size optimization exceeded the C2 boundary. Release builds now
  use size optimization/LTO for higher-level code; Furi, Cortex-M FreeRTOS ports
  and startup/interrupt code remain non-LTO because of naked assembly literal
  pools. External FAP compilation stays non-LTO. The whole matched resource/app
  package is rebuilt. Existing upstream startup-signature LTO warnings remain
  visible; a successful build is not a runtime certification.
- 16 project Python tests pass: identity binding, allowlist behavior, expiry/
  future credentials, revocation, final job results, timeout cancellation and
  the real configuration route refusing anonymous token issuance before reading
  an unauthenticated request body, plus stale-build-result rejection.
- The `deepflipper-s3` reference variant builds and merges under IDF v6.1/GCC
  15.2. It defaults to manual start/stop, no wake word and no AFE processing.
  The EIM wrapper validates a per-build nonce and merged-image hash, since the
  EIM outer process may report success even when the child compiler fails.
- Native C++ RPC codec test passes golden bytes, maximum length, malformed
  varints, truncated messages and rejected RPC command status.
- Nine relevant XiaoZhi board/menu/configuration tests pass, including the new
  board's menu ordering and build variant constraints.
- A broader inherited 140-test host run was attempted: 12 environment-dependent
  errors remain on Windows (temporary-directory working-directory cleanup,
  CI Bash/Git execution and unavailable host test compiler), with 19 skipped.
  This run is not presented as passing or as an ESP32 target build.
- Sprite data, manifests and C data exports are checked independently. Tests
  and output details are recorded in `build/validation.json` when exported.

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
