# Arduino examples

[中文](README_ZH.md)

These ten sketches target ESP32-P4 Rev3.1 or newer with Arduino-ESP32 `3.3.11`.
The tested core profile is `ChipVariant=postv3` with PSRAM enabled, 32 MB flash,
QIO at 80 MHz, and the 13 MB application / 7 MB data partition layout. That
prebuilt core uses 200 MHz PSRAM; it is not the ESP-IDF Rev3.x 250 MHz profile
and does not support Rev3.0.

The repository keeps the complete Arduino library bundles used by the examples,
including their upstream licenses, documentation, and source trees:

- `GFX Library for Arduino` `1.6.0`
- `lvgl` `9.3.0` plus the full `lv_conf.h` configuration for example 04

Do not replace these with a Library Manager download when reproducing the
reviewed build. The Actions workflow compiles directly against the checked-in
copies so local and CI builds use the same sources.

The sketches also require this repository's board-specific `libraries/displays`
adapter. With Arduino IDE, copy `GFX_Library_for_Arduino`, `lvgl`, `lv_conf.h`,
and `displays` from `examples/arduino/libraries/` into the sketchbook
`libraries/` directory without trimming their contents. With Arduino CLI from
the repository root, pass the complete directory explicitly:

```console
arduino-cli compile --fqbn <esp32p4-fqbn> \
  --libraries examples/arduino/libraries \
  examples/arduino/examples/01_HelloWorld
```

The board-specific `libraries/displays` adapter contains this board's 480 × 800
ST7701 configuration. It uses a 30 MHz DPI clock, two 500 Mbps MIPI-DSI lanes,
and ESP-IDF's revision-aware default MIPI PHY clock source. GT911 is polled
without assigning INT or RST: initialization probes `0x5D`, then `0x14`, and
uses the address that responds.

| Example | Purpose | Hardware prerequisite |
| --- | --- | --- |
| `01_HelloWorld` | Display color bars, text, and drawing | Board |
| `02_AsciiTable` | Character and layout test | Board |
| `03_Drawing_board` | Five-point GT911 drawing | Board touch panel |
| `04_LVGLV9_Arduino` | LVGL 9 widgets and touch | Board touch panel |
| `05_GFX_ESPWiFiAnalyzer` | Wi-Fi channel visualization | Onboard ESP32-C6 wireless path |
| `06_Camera_Preview` | OV5647 MIPI-CSI preview | Compatible OV5647 camera |
| `07_Camera_ISP_Tuning` | Serial-controlled camera ISP tuning | Compatible OV5647 camera |
| `08_SD_Card` | Four-line SDMMC mount and file test | microSD card |
| `09_Audio_Playback` | ES8311 melody playback | Speaker connected to the board output |
| `10_Mic_Record` | ES7210 PCM capture statistics | Onboard microphones |

The published product material and checked-in schematic do not identify an
onboard CAN or RS485 transceiver or assign verified transceiver pins. Use an
external physical layer only after confirming its wiring; these examples do
not invent a CAN/RS485 pin map.

Compilation proves API and source compatibility only. Display, touch, camera,
audio, storage, wireless, MIPI timing, and long-run behavior still require
testing on the matching board revision and connected accessories.
