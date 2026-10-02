# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

RetroESP32-P4: a multi-emulator + native-app platform for the ESP32-P4 (dual-core RISC-V, 32 MB PSRAM, 16 MB flash), built with **ESP-IDF v5.5.2**. Board targets from one source tree: **LCD** (Guition 4.3″ 480×800 ST7701S + GT911 touch), **HDMI** (Olimex LT8912 DSI→HDMI, 640×480) and **Waveshare** (ESP32-P4-WIFI6-Touch-LCD-4.3, same panel class, different pins). `ARCHITECTURE.md` is the detailed reference (if it disagrees with code, code wins); `PAPP_GUIDE.md` covers writing PSRAM apps. Docs referenced there (`DEVELOPMENT_LOG.md`, `PSRAM_APP.md`, `Neogeo.md`, `HDMIport.md`) are not in this repo.

There is no test suite or linter. Verification is building and running on hardware. `ESP32-P4-WIFI6-Touch-LCD-4.3/` is Waveshare's vendor reference material (examples, schematic) and the source for the Waveshare pin map; it is not part of the firmware build.

## Build & flash (Windows-centric, PowerShell/batch)

The scripts assume Windows and a local ESP-IDF install; `tools/resolve_idf_env.bat` / `tools/Resolve-IdfEnv.ps1` activate it (honoring `IDF_PATH` / `IDF_PYTHON_ENV_PATH`).

- Full LCD build: `build_all.ps1` (or `build_all_lcd.bat`) → `firmware/`, then `generate_merged_bin.ps1` → `RetroESP32_P4_v1.bin`
- Full HDMI build: `build_all_hdmi.bat` / `build_hdmi.ps1` → `firmware_hdmi/` → `RetroESP32_P4_HDMI_v1.bin`
- Flash everything: `flash_all.ps1` (set `$PORT`); or flash a merged bin at address `0`
- Single project: `cd apps\snes; idf.py build` (or `cd launcher`). **Delete that project's `sdkconfig` and `build/` first if it was last built for the other target** — a stale `CONFIG_HDMI_OUTPUT=y` gives a black screen and broken audio. The build-all scripts do this automatically.
- PAPP apps: `tools\build_psram_app.ps1 -AppName X -Sources ...` (per-port scripts: `tools/build_{doom,quake,duke3d,opentyrian,lvgl}_papp.ps1`), upload with `tools/upload_papp.py`. Built `.papp` files must also be committed to `SDcard/roms/papp/` (the git-tracked mirror of the SD card).
- Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 build: `build_waveshare.ps1` (add `-Rev1` for Rev1.x silicon) → `firmware_waveshare/` → `RetroESP32_P4_Waveshare_v1.bin`. Board selection is the compile-time Kconfig `CONFIG_BOARD_WAVESHARE_P4_43` (`launcher/sdkconfig.waveshare.defaults`); Rev3.x chips also need `launcher/sdkconfig.p4rev3.defaults` because the default images are limited to chip rev ≤1.99. Pin map: `components/odroid/include/pins_config.h` (keep the identical copies in `components/app_common/include/` and `launcher/main/` in sync); not yet hardware-validated.
- Neo Geo cache: `python SDcard/roms/neogeo/gen_cache.py <game>` (root `gen_ctile.py` / `gen_vrom.py` are the underlying generators; `verify_ctile.py` checks output).

## Architecture (big picture)

**Two execution models:**
1. **OTA multi-binary** — the launcher (`launcher/`, `factory` partition) and each emulator (`apps/<emu>/`, one per `ota_N` partition) are *separate ESP-IDF projects/firmwares*. Launching a ROM writes `RomFilePath`/`DataSlot` to NVS (namespace `"Odroid"`), sets the OTA boot partition chosen by `get_ota_slot(ext)` in `launcher/main/main.c`, and reboots; exiting sets boot=factory and reboots. The extension→slot map lives only in that function.
2. **PSRAM `.papp`** — native ports (Doom, Quake, Duke3D, OpenTyrian) are flat binaries loaded from SD into PSRAM, MMU-mapped executable at `0x4A000000`, and called via `app_entry(const app_services_t*)`. They never call ESP-IDF/libc directly; everything goes through the `app_services_t` function-pointer table. **`components/psram_app_loader/include/psram_app.h` is the ABI source of truth.** Return from `app_entry` goes back to the launcher with no reboot. Templates: `ESP32_P4_PAPP_Template/`; ports: `apps/psram_*/`.

**Layout:** `components/odroid/` is the HAL (system, audio, display, input, sdcard, settings); `components/app_common/` is the standard OTA-app lifecycle (`app_init`, `app_get_rom_path`, `app_return_to_launcher`); emulator cores are components (`nofrendo gnuboy smsplus spectrum stella prosystem handy huexpress atari800 snes9x gwenesis gngeo`) and each `apps/<emu>/main/<emu>_run.c` holds per-system glue (audio, input mapping, scaling) where most tuning happens. `main/` at repo root is a legacy monolithic build — unused.

**Constraints to remember:**
- `partitions_ota.csv` is shared by launcher + all apps; flash offsets **must match** the `$flash_map` in `flash_all.ps1` and the merge scripts. Growing a slot means shifting every later offset. Launcher, NES and Atari800 slots are nearly full. `ota_9` is free.
- ESP32-P4 bootloader is at `0x2000`, partition table at `0x8000`.
- Emulator sdkconfig comes from `apps/sdkconfig_common.defaults`; launcher from `launcher/sdkconfig.defaults` (+ `sdkconfig.hdmi.defaults`). `CONFIG_FATFS_LFN_HEAP=y` / `MAX_LFN=255` are required or ROM names are 8.3-truncated.
- `odroid_input_gamepad_read()` ORs the onboard GPIO pad (pins 28–35) with the USB HID pad; `odroid_input.c` does per-pin stuck detection for floating pins. Holding **A** in the first ~500 ms of an emulator boot safe-boots back to the launcher.
- Neo Geo does not run from the zip alone: it needs precomputed `.ctile/.cusage/.vroma/.vromb/.sfix` cache files per game on the SD card.

**Adding an emulator:** core component exposing `<core>_run(rom_path)` → `apps/<core>/` project (`REQUIRES app_common <core>`) → register extensions in `get_ota_slot()` and the browser filter in `launcher/main/main.c` → verify size vs slot → add to `build_all*.{ps1,bat}`, `flash_all.ps1`, and merge scripts.

`GamePadSetUp.txt` is a spec for a custom GPIO/analog gamepad (ADC pins 49/50/51/52, GPIO 28–34, detected via pull-up on IO 29) to run in parallel with the USB pad — it's a task note, not yet documentation of existing behavior.
