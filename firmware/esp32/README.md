# DeepFlipper ESP32-S3 voice firmware

Firmware for the DeepFlipper voice module: an ESP32-S3 (16 MB flash, 8 MB
octal PSRAM) that captures speech, talks to the DeepFlipper server over
WebSocket and plays the reply, while the Flipper Zero renders the character
over the expansion UART link.

Only one board is built from this tree: `main/boards/deepflipper-s3/`.
Its pins, links and limits are described in that directory's
[README](main/boards/deepflipper-s3/README.md).

## SDK

ESP-IDF `release/v6.1` is recommended; the minimum is ESP-IDF v6.0.1.
ESP-IDF 5.x is not supported.

## Build

On Windows, from the repository root:

```powershell
.\scripts\build_esp32.ps1
```

Inside an activated ESP-IDF shell, from `firmware/esp32`:

```sh
python3 scripts/build.py deepflipper-s3 --name deepflipper-s3
```

The application image is `build/deepflipper.bin` (flashed at `0x20000`);
`build/merged-binary.bin` is the full image for `0x0`.

## Documentation

- Repository overview and release/flash steps: [../../README.md](../../README.md)
- Implementation notes: [../../docs/implementation.md](../../docs/implementation.md)
- Hardware notes: [../../hardware/README.md](../../hardware/README.md)
- Audio pipeline: [main/audio/README.md](main/audio/README.md)
- Device protocols: [docs/websocket.md](docs/websocket.md),
  [docs/mcp-protocol.md](docs/mcp-protocol.md), [docs/mqtt-udp.md](docs/mqtt-udp.md)
- Code style: [docs/code_style.md](docs/code_style.md)
- Contributor/agent rules: [AGENTS.md](AGENTS.md)

The WebSocket path `/xiaozhi/v1/` is part of the server protocol and is kept
for compatibility.

## Origin and license

Forked from [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT).
Unused upstream boards, locales and release tooling have been removed. See
[LICENSE](LICENSE).
