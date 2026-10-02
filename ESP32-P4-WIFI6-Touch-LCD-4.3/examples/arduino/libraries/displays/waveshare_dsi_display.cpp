/*
 * SPDX-FileCopyrightText: 2026 Waveshare Electronics
 * SPDX-License-Identifier: Apache-2.0
 *
 * The initialization order follows Arduino_GFX's ESP32-P4 DSI implementation,
 * with the LCD-4.3 BSP's timing, PHY clock selection, and framebuffer count.
 */
#include "waveshare_dsi_display.h"

#include <driver/gpio.h>
#include <esp_cache.h>
#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace {
constexpr int kDsiPhyLdoChannel = 3;
constexpr int kDsiPhyLdoVoltageMv = 2500;
constexpr uint32_t kHsyncPulse = 12;
constexpr uint32_t kHsyncBackPorch = 42;
constexpr uint32_t kHsyncFrontPorch = 42;
constexpr uint32_t kVsyncPulse = 8;
constexpr uint32_t kVsyncBackPorch = 2;
constexpr uint32_t kVsyncFrontPorch = 60;
}

WaveshareDsiDisplay::WaveshareDsiDisplay(int16_t width, int16_t height,
                                         int8_t reset_pin, uint32_t dpi_clock_hz,
                                         uint32_t lane_bit_rate_mbps,
                                         const WaveshareLcdInitCmd *init_commands,
                                         size_t init_command_count)
    : Arduino_GFX(width, height), reset_pin_(reset_pin), dpi_clock_hz_(dpi_clock_hz),
      lane_bit_rate_mbps_(lane_bit_rate_mbps), init_commands_(init_commands),
      init_command_count_(init_command_count)
{
    setRotation(0);
}

bool WaveshareDsiDisplay::begin(int32_t)
{
    esp_ldo_channel_handle_t ldo = nullptr;
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = kDsiPhyLdoChannel;
    ldo_config.voltage_mv = kDsiPhyLdoVoltageMv;
    if (esp_ldo_acquire_channel(&ldo_config, &ldo) != ESP_OK) return false;

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    // 0 lets ESP-IDF select PLL_F20M on pre-v3 and XTAL on rev3.x silicon.
    bus_config.phy_clk_src = static_cast<mipi_dsi_phy_pllref_clock_source_t>(0);
    bus_config.lane_bit_rate_mbps = lane_bit_rate_mbps_;
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    if (esp_lcd_new_dsi_bus(&bus_config, &dsi_bus) != ESP_OK) return false;

    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &panel_io) != ESP_OK) return false;

    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = dpi_clock_hz_ / 1000000UL;
    dpi_config.virtual_channel = 0;
    dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = WIDTH;
    dpi_config.video_timing.v_size = HEIGHT;
    dpi_config.video_timing.hsync_back_porch = kHsyncBackPorch;
    dpi_config.video_timing.hsync_pulse_width = kHsyncPulse;
    dpi_config.video_timing.hsync_front_porch = kHsyncFrontPorch;
    dpi_config.video_timing.vsync_back_porch = kVsyncBackPorch;
    dpi_config.video_timing.vsync_pulse_width = kVsyncPulse;
    dpi_config.video_timing.vsync_front_porch = kVsyncFrontPorch;
    dpi_config.flags.use_dma2d = true;
    esp_lcd_panel_handle_t panel = nullptr;
    if (esp_lcd_new_panel_dpi(dsi_bus, &dpi_config, &panel) != ESP_OK) return false;

    // The target BSP leaves reset_active_high unset, so ST7701 is reset low.
    if (reset_pin_ >= 0) {
        pinMode(reset_pin_, OUTPUT);
        digitalWrite(reset_pin_, LOW);
        delay(10);
        digitalWrite(reset_pin_, HIGH);
        delay(10);
    }
    for (size_t i = 0; i < init_command_count_; ++i) {
        const WaveshareLcdInitCmd &command = init_commands_[i];
        if (esp_lcd_panel_io_tx_param(panel_io, command.cmd, command.data,
                                      command.data_bytes) != ESP_OK) return false;
        if (command.delay_ms) vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
    }
    if (esp_lcd_panel_init(panel) != ESP_OK) return false;

    void *framebuffer = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &framebuffer) != ESP_OK ||
        framebuffer == nullptr) return false;
    framebuffer_ = static_cast<uint16_t *>(framebuffer);
    return true;
}

void WaveshareDsiDisplay::flush(void *address, size_t length)
{
    esp_cache_msync(address, length,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void WaveshareDsiDisplay::write_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (!framebuffer_) return;
    uint16_t *pixel = nullptr;
    switch (_rotation) {
    case 1: pixel = framebuffer_ + static_cast<int32_t>(x) * WIDTH + (WIDTH - 1 - y); break;
    case 2: pixel = framebuffer_ + static_cast<int32_t>(HEIGHT - 1 - y) * WIDTH + (WIDTH - 1 - x); break;
    case 3: pixel = framebuffer_ + static_cast<int32_t>(HEIGHT - 1 - x) * WIDTH + y; break;
    default: pixel = framebuffer_ + static_cast<int32_t>(y) * WIDTH + x; break;
    }
    *pixel = color;
    flush(pixel, sizeof(*pixel));
}

void WaveshareDsiDisplay::writePixelPreclipped(int16_t x, int16_t y, uint16_t color)
{
    write_pixel(x, y, color);
}

void WaveshareDsiDisplay::writeFillRectPreclipped(int16_t x, int16_t y, int16_t w,
                                                   int16_t h, uint16_t color)
{
    // The common portrait orientations map each row to a contiguous framebuffer
    // run, so sync one cache range per row rather than once per pixel.
    if (framebuffer_ && (_rotation == 0 || _rotation == 2)) {
        for (int16_t row = 0; row < h; ++row) {
            uint16_t *first = nullptr;
            if (_rotation == 0) {
                first = framebuffer_ + static_cast<int32_t>(y + row) * WIDTH + x;
            } else {
                first = framebuffer_ + static_cast<int32_t>(HEIGHT - 1 - (y + row)) * WIDTH +
                        (WIDTH - x - w);
            }
            for (int16_t column = 0; column < w; ++column) first[column] = color;
            flush(first, static_cast<size_t>(w) * sizeof(*first));
        }
        return;
    }
    for (int16_t row = 0; row < h; ++row)
        for (int16_t column = 0; column < w; ++column)
            write_pixel(x + column, y + row, color);
}

void WaveshareDsiDisplay::draw_bitmap(int16_t x, int16_t y,
                                      const uint16_t *bitmap, int16_t w, int16_t h)
{
    if (!framebuffer_ || !bitmap || w <= 0 || h <= 0) return;

    const int16_t source_width = w;
    int16_t source_x = 0;
    int16_t source_y = 0;
    if (x < 0) {
        source_x = -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        source_y = -y;
        h += y;
        y = 0;
    }
    if (x >= _width || y >= _height) return;
    if (x + w > _width) w = _width - x;
    if (y + h > _height) h = _height - y;
    if (w <= 0 || h <= 0) return;

    if (_rotation == 0) {
        for (int16_t row = 0; row < h; ++row) {
            uint16_t *destination = framebuffer_ +
                static_cast<int32_t>(y + row) * WIDTH + x;
            const uint16_t *source = bitmap +
                static_cast<int32_t>(source_y + row) * source_width + source_x;
            memcpy(destination, source, static_cast<size_t>(w) * sizeof(*destination));
            flush(destination, static_cast<size_t>(w) * sizeof(*destination));
        }
        return;
    }

    // Other orientations are uncommon for this portrait panel. Preserve the
    // Arduino_GFX rotation contract while still batching each touched row in
    // the common rotation-2 case.
    if (_rotation == 2) {
        for (int16_t row = 0; row < h; ++row) {
            uint16_t *destination = framebuffer_ +
                static_cast<int32_t>(HEIGHT - 1 - (y + row)) * WIDTH +
                (WIDTH - x - w);
            const uint16_t *source = bitmap +
                static_cast<int32_t>(source_y + row) * source_width + source_x;
            for (int16_t column = 0; column < w; ++column) {
                destination[w - 1 - column] = source[column];
            }
            flush(destination, static_cast<size_t>(w) * sizeof(*destination));
        }
        return;
    }

    for (int16_t row = 0; row < h; ++row) {
        const uint16_t *source = bitmap +
            static_cast<int32_t>(source_y + row) * source_width + source_x;
        for (int16_t column = 0; column < w; ++column) {
            write_pixel(x + column, y + row, source[column]);
        }
    }
}

void WaveshareDsiDisplay::draw16bitRGBBitmap(int16_t x, int16_t y,
                                              const uint16_t bitmap[],
                                              int16_t w, int16_t h)
{
    draw_bitmap(x, y, bitmap, w, h);
}

void WaveshareDsiDisplay::draw16bitRGBBitmap(int16_t x, int16_t y,
                                              uint16_t *bitmap,
                                              int16_t w, int16_t h)
{
    draw_bitmap(x, y, bitmap, w, h);
}
