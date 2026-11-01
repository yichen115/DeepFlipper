# Reference module wiring

This is a reference configuration for a **bare ESP32-S3 with 16 MB flash and
8 MB octal PSRAM**. It is not a verified schematic for the user's development
board. Confirm pin availability, electrical ratings and the audio slot wiring
before powering or flashing. The firmware board ID is `deepflipper-s3`.

| Signal | Flipper | S3 reference GPIO |
|---|---|---|
| Module supply | top pin 1, 5 V | protected/switched input, then local regulation |
| Ground | top pin 11 (also 8) | GND |
| Flipper TX -> module RX | top pin 13 / PB6 | 18 |
| Module TX -> Flipper RX | top pin 14 / PB7 | 17 |
| I2S mic WS / BCLK / data | not used | 4 / 5 / 6 |
| I2S amp data / BCLK / WS | not used | 7 / 15 / 16 |
| WS2812 DIN (onboard or external) | not used | 48, one GRB pixel |
| Local talk button | not used | 0, active-low (BOOT strap: do not hold during reset) |

`firmware/esp32/main/boards/deepflipper-s3/config.h` owns this mapping. Microphone
and amp should match the reference I2S codec configuration. Speaker requires a
proper amp. The `deepflipper-s3` build drives one WS2812 through
RMT on GPIO48; no LED wires are needed if the board already has this connection.
It does not drive GPIO8/9/10. Some S3 boards instead use GPIO38; verify the board
schematic. The S3 chip itself does not contain an RGB LED. Web brightness (0–100)
controls the onboard light; zero sends black but does not cut its physical supply.
External indicators also use WS2812 with DIN on GPIO48 and a rated supply/common
ground; there is no three-channel PWM RGB driver. GPIO8/9/10 are free.

The DeepFlipper speaker output now uses standard Philips I2S, 32-bit left/right
slots with identical mono PCM in both. This avoids attenuating the signal when
a mono amplifier is strapped to average L/R, and supports either channel select.
The microphone remains on its own I2S controller with the original pins/format.
Software volume still applies to both slots and zero remains silent. After an
update, start around 30–50% volume because local TTS speech is level-normalized.

If using a MAX98357A module, use its rated supply and a common ground with the S3;
keep BCLK/WS/DIN and ground wiring short. Its speaker connects across SPK+/SPK−,
not from a speaker terminal to ground. A weak supply, poor ground or amplifier
gain/SD strap can cause noise or low level even when digital PCM is correct.
The source and host tests cannot establish the actual amplifier/rail condition.

The physical power switch must cover ESP32, microphone/codec, amp and LED.
Use power-off-high-impedance isolation on UART/other cross-domain signals to
avoid phantom power. Do not directly parallel USB and Flipper 5 V. The software
AI switch releases the Expansion link but does not disconnect this hardware rail
or globally disable another accessory's 5 V supply.

Reserve antenna clearance, mic/speaker separation, debug/BOOT/RESET access and
test points. Peak current, inrush, low-battery behavior, power-off leakage,
audio noise, RF performance and Flipper compatibility require bench validation.
No PCB, BOM, protection circuit or load rating is supplied by this reference map.

## Debug console versus Flipper UART

Bluetooth is already disabled in the S3 build. ESP32-S3 is BLE-only and cannot
host normal Classic HFP/A2DP headsets. Power changes enable 80–240 MHz CPU DFS,
restore modem sleep in idle voice state, and provide manual UART-wakeable Wi-Fi
standby. The active Flipper Expansion worker still holds insomnia; power savings
must be measured, and hardware module power-off is needed for baseline autonomy.

The Expansion link carries binary framing/protobuf around JSON; a text terminal
will show control characters even at the correct rate. Repeated hello replies
with a duplicated/unquoted `native_rpc` suffix indicate the old Flipper Frozen
formatter bug: install the current Flipper update package. GPIO17/18 traffic and
the UART0 boot console are separate; capture the latter for `FlipperLink` logs.

The Flipper link is **UART1, TX GPIO17 / RX GPIO18**. On a generic S3 development
board, header pins marked TX/RX normally belong to the separate **UART0 console,
GPIO43/GPIO44 at 115200**. Wire Flipper pin 13 TX to S3 IO18 RX, Flipper pin 14 RX
to S3 IO17 TX, and share GND. GPIO numbers are not the physical header positions.
Seeing boot/Wi-Fi text on CH340 confirms the console path; the Flipper path carries
binary Expansion frames and is independent of that text console.

For receive-only console monitoring, connect CH340 RX to S3 GPIO43 and GND to GND;
leave CH340 TX/VCC unconnected while the module already has its own supply. Keep
the console at 115200 even when the Flipper link negotiates 460800. Avoid putting
two TX outputs on the same net. New firmware logs `FlipperLink` handshake stages
and RX/error counters without credentials. `no_heartbeat` is before rate negotiation:
check IO17/18, common ground, AI session and Expansion listener. `baud_ack_failed`
means negotiation was not acknowledged; `rpc_start_failed`/`hello_rejected` locate
later stages. A stable connection logs `Expansion connected at ... baud`.
