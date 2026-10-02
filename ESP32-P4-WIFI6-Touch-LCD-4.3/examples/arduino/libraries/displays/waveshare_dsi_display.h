/*
 * SPDX-FileCopyrightText: 2026 Waveshare Electronics
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal Arduino_GFX framebuffer adapter for ESP32-P4 MIPI-DSI panels.
 * It uses ESP-IDF's public ESP LCD APIs bundled with Arduino-ESP32 3.3.11.
 */
#pragma once

#include <Arduino_GFX_Library.h>

struct WaveshareLcdInitCmd {
    int cmd;
    const uint8_t *data;
    size_t data_bytes;
    uint32_t delay_ms;
};

class WaveshareDsiDisplay : public Arduino_GFX {
public:
    WaveshareDsiDisplay(int16_t width, int16_t height, int8_t reset_pin,
                        uint32_t dpi_clock_hz, uint32_t lane_bit_rate_mbps,
                        const WaveshareLcdInitCmd *init_commands,
                        size_t init_command_count);

    bool begin(int32_t speed = GFX_NOT_DEFINED) override;
    void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override;
    void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                                 uint16_t color) override;
    void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t bitmap[],
                            int16_t w, int16_t h) override;
    void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t *bitmap,
                            int16_t w, int16_t h) override;

private:
    void draw_bitmap(int16_t x, int16_t y, const uint16_t *bitmap,
                     int16_t w, int16_t h);
    void write_pixel(int16_t x, int16_t y, uint16_t color);
    void flush(void *address, size_t length);

    int8_t reset_pin_;
    uint32_t dpi_clock_hz_;
    uint32_t lane_bit_rate_mbps_;
    const WaveshareLcdInitCmd *init_commands_;
    size_t init_command_count_;
    uint16_t *framebuffer_ = nullptr;
};
