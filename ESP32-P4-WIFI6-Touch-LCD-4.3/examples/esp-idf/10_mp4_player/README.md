# MP4/AVI player on the onboard LCD

[中文](README_ZH.md)

This example reads a media file from the onboard MicroSD card, decodes it on
ESP32-P4, renders video on the 4.3-inch 480 × 800 MIPI-DSI LCD, and sends audio
to the board codec when an audio device is available. No HDMI bridge is used.

## Supported media paths

- MP4 and AVI container extractors are registered.
- MJPEG is the supported video codec. H.264 and other video codecs are rejected
  by the current compatibility check.
- AAC and MP3 audio decoders are always registered.
- FLAC, Opus, Vorbis, and ADPCM decoders can be enabled in `menuconfig`.
- If audio-device initialization fails, the application continues with video
  playback.

The default media path is `/sdcard/test_video.mp4`. The player restarts the file
after playback ends. Use media whose dimensions and throughput fit the display
and available PSRAM bandwidth; actual playback stability must be verified on
the board.

## Prepare the MicroSD card

1. Format a card with a filesystem supported by the ESP-IDF FAT filesystem
   component.
2. Copy an MJPEG video to the card.
3. Name it `test_video.mp4`, or change `Video File Name` in the
   `MP4/AVI Player Configuration` menu.
4. Insert the card before starting the application.

## Build, flash, and monitor

From this directory in an activated ESP-IDF environment:

```console
idf.py set-target esp32p4
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's USB-to-UART port. Press `Ctrl-]` to leave the
monitor. The repository Actions workflow compiles this example with ESP-IDF
`v5.5.5` and `v6.0.2`, including RGB565 and RGB888 BSP paths. CI does not test
media compatibility, sustained throughput, audio quality, or hardware playback.
