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
| RGB R / G / B | not used | 8 / 9 / 10 |
| Local talk button | not used | 0, active-low (BOOT strap: do not hold during reset) |

`firmware/esp32/main/boards/deepflipper-s3/config.h` owns this mapping. Microphone
and amp should match the reference I2S codec configuration. Speaker requires a
proper amp; RGB reference is a common-cathode LED with a separate resistor per
color and any required current drivers. GL5050RGB01H-T is not confirmed as the
selected part. A common-anode or addressable LED requires a matching driver.

The physical power switch must cover ESP32, microphone/codec, amp and LED.
Use power-off-high-impedance isolation on UART/other cross-domain signals to
avoid phantom power. Do not directly parallel USB and Flipper 5 V. The software
AI switch releases the Expansion link but does not disconnect this hardware rail
or globally disable another accessory's 5 V supply.

Reserve antenna clearance, mic/speaker separation, debug/BOOT/RESET access and
test points. Peak current, inrush, low-battery behavior, power-off leakage,
audio noise, RF performance and Flipper compatibility require bench validation.
No PCB, BOM, protection circuit or load rating is supplied by this reference map.
