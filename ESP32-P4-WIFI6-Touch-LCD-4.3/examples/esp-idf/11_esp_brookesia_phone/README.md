# ESP-Brookesia phone demo

[中文](README_ZH.md)

This example runs an ESP-Brookesia phone-style shell and a locally registered
Squareline application on the ESP32-P4-WIFI6-Touch-LCD-4.3. It starts the board
display and touch stack, installs applications from the Brookesia registry, and
updates the status-bar clock.

The required `brookesia_core` and `brookesia_app_squareline_demo` sources are
already included under `components/`; cloning another ESP-Brookesia repository
is not part of this example's build procedure.

## Default configuration

The checked-in `sdkconfig.defaults` selects ESP32-P4, 32 MB QIO flash, PSRAM at
200 MHz, and three display buffers. To keep the default footprint bounded, it
disables the Brookesia AI framework, animation player, services, speaker system,
and unused Boost libraries. Actions also compiles an explicit AI-enabled lane
with both ESP-IDF `v5.5.5` and `v6.0.2` so that its conditional dependency path
does not silently regress on either supported IDF line.

ESP-Brookesia in this repository is a preview integration. Actions proves that
the code compiles with ESP-IDF `v5.5.5` and `v6.0.2`; layout, touch behavior,
performance, and optional services still require testing on the 480 × 800 target
board.

## Build, flash, and monitor

From this directory in an activated ESP-IDF environment:

```console
idf.py set-target esp32p4
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's USB-to-UART port. Press `Ctrl-]` to leave the
monitor. Use `idf.py menuconfig` before building if you want to enable optional
Brookesia subsystems; those features can add managed-component dependencies and
increase flash, PSRAM, and task requirements.

For issues specific to this product adaptation, use this repository's
[issue tracker](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3/issues).
