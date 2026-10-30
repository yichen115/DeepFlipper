# DeepFlipper S3 reference board

Dedicated board identity for the GPIO-connected Flipper audio module. This is
a bare ESP32-S3 reference map with 16 MB flash and 8 MB octal PSRAM, not a claim
that every XiaoZhi/S3 development board has compatible wiring.

- USB-independent UART1 link: module TX GPIO17, RX GPIO18; see `config.h`.
- Expansion discovery at 9600, negotiated 460800 preferred; failed setup or a
  lost established link latches 115200 until module reboot. Status exposes the
  selected rate/fallback; physical high-speed reliability is not yet verified.
- I2S microphone and amp, separate input/output clocks, 16 kHz input/24 kHz output.
- Single common-cathode RGB with per-channel resistors/drivers, three LEDC outputs.
- GPIO0 active-low local talk button: click to start, click again to finish.
- No local LCD/OLED or conversation captions. Flipper renders the character.
- Direct WS/WSS device credentials; no managed OTA activation/configuration overwrite.
- Wake word, AFE, device/server AEC and asset flashing are off for this initial
  button-controlled variant. Enabling them requires matching models and hardware
  verification, not just an extra UI switch.

For a trusted LAN, use `ws://PC_IP:8000/xiaozhi/v1/` without a domain or certificate.
Device tokens are still required. Public deployments should use WSS; TLS checks
remain enabled and there is no automatic WSS-to-WS fallback.

See the project's [reference hardware notes](../../../../../hardware/README.md)
for power switching, UART isolation and the complete map. No schematic/PCB or
physical validation is included.

Inside an ESP-IDF 6.1 environment, from `firmware/esp32`:

```text
python scripts/build.py deepflipper-s3 --name deepflipper-s3
```

On Windows, prefer project `scripts/build_esp32.ps1`, which runs that canonical
entry through EIM and validates the actual build result before exporting files.

Device configuration runs at `http://192.168.4.1` on the protected setup AP and
at the assigned device IP on LAN. Flipper **AI Assistant → Setup: …** shows
the current address, AP SSID and device-specific password (also the initial web
login password). Configure Wi-Fi in the browser, then import a connection JSON
exported by the Python admin. No long text entry on Flipper is needed. The
board's setup task owns AP/STA, scan/reconnect, revisioned profile persistence,
sessions and server diagnostics; no managed component sources are modified.
After normal boot, holding the talk button for 5 seconds restores the web login
password and opens the AP while preserving all network/server credentials.
