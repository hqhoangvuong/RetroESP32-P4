# Camera preview on the onboard LCD

[中文](README_ZH.md)

This example captures frames from a MIPI-CSI camera and displays them on the
onboard 4.3-inch 480 × 800 ST7701 MIPI-DSI panel of the
ESP32-P4-WIFI6-Touch-LCD-4.3.

The default configuration selects an OV5647 MIPI sensor mode using RAW8 at
800 × 1280 and 50 fps. The application obtains the negotiated camera size from
the V4L2 device, converts frames with the ESP32-P4 PPA, and center-crops them to
the LCD. It does not scale the image. Camera orientation and the usable field of
view therefore depend on the connected module and its installation.

## Requirements

- An ESP32-P4-WIFI6-Touch-LCD-4.3.
- A compatible OV5647 camera connected to the board's 15-pin MIPI-CSI socket.
- ESP-IDF `v5.5.5` or `v6.0.2`, the versions compiled by this repository's
  Actions workflow.
- A USB connection for power, flashing, and serial output.

Confirm camera orientation, cable contact direction, and module compatibility
against the [official product documentation](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3)
before powering the board.

## Configuration

The checked-in `sdkconfig.defaults` selects ESP32-P4, 32 MB flash, PSRAM at
200 MHz, three LCD frame buffers, OV5647 support, and the I2C pins used by the
board BSP. The BSP color-format choice supports RGB565 (default) and RGB888;
both compile paths are exercised in Actions.

Use `idf.py menuconfig` to review the camera sensor mode and the board support
package settings before flashing a different camera configuration.

## Build, flash, and monitor

From this directory in an activated ESP-IDF environment:

```console
idf.py set-target esp32p4
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's USB-to-UART port. Press `Ctrl-]` to leave the
monitor. A successful Actions build proves compile compatibility only; camera
signal integrity, image orientation, and display timing still require testing
on the target hardware.
