/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Waveshare Electronics
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from the public esp_lcd_touch_gt911 transport flow. This board has
 * neither an Arduino-controlled reset nor interrupt line, so initialization
 * probes both strap-selected GT911 addresses and reads are polling-only.
 */
#include "gt911.h"

#include <driver/i2c_master.h>
#include <esp_lcd_io_i2c.h>
#include <esp_lcd_panel_io.h>

#include "displays_config.h"

namespace {
constexpr uint8_t kGt911Address = 0x5D;
constexpr uint8_t kGt911BackupAddress = 0x14;
constexpr uint16_t kGt911ProductIdRegister = 0x8140;
constexpr uint16_t kGt911ConfigVersionRegister = 0x8047;
constexpr uint16_t kGt911PointRegister = 0x814E;
constexpr uint8_t kMaxPoints = 5;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_lcd_panel_io_handle_t s_touch_io = nullptr;

bool tx(uint16_t reg, const void *data, size_t length)
{
    return esp_lcd_panel_io_tx_param(s_touch_io, reg, data, length) == ESP_OK;
}

bool rx(uint16_t reg, void *data, size_t length)
{
    return esp_lcd_panel_io_rx_param(s_touch_io, reg, data, length) == ESP_OK;
}
}

bool gt911_begin()
{
    if (s_touch_io) return true;

    i2c_master_bus_config_t bus_config = {};
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.sda_io_num = static_cast<gpio_num_t>(LCD43_SDA);
    bus_config.scl_io_num = static_cast<gpio_num_t>(LCD43_SCL);
    bus_config.i2c_port = -1;
    if (!s_i2c_bus && i2c_new_master_bus(&bus_config, &s_i2c_bus) != ESP_OK) return false;

    uint8_t address = 0;
    if (i2c_master_probe(s_i2c_bus, kGt911Address, 100) == ESP_OK) address = kGt911Address;
    else if (i2c_master_probe(s_i2c_bus, kGt911BackupAddress, 100) == ESP_OK) address = kGt911BackupAddress;
    else return false;

    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = address;
    io_config.scl_speed_hz = 100000;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    if (esp_lcd_new_panel_io_i2c_v2(s_i2c_bus, &io_config, &s_touch_io) != ESP_OK) return false;

    // Match the managed GT911 driver's initialization contract: selecting an
    // address is not sufficient. Verify that the controller's identification
    // and configuration registers can be read through the selected address.
    uint8_t product_id[3] = {};
    uint8_t config_version = 0;
    if (!rx(kGt911ProductIdRegister, product_id, sizeof(product_id)) ||
        !rx(kGt911ConfigVersionRegister, &config_version, sizeof(config_version))) {
        esp_lcd_panel_io_del(s_touch_io);
        s_touch_io = nullptr;
        return false;
    }
    return true;
}

bool gt911_read(Gt911Point &point)
{
    point = {};
    if (!s_touch_io) return false;

    uint8_t status = 0;
    if (!rx(kGt911PointRegister, &status, sizeof(status))) return false;
    if ((status & 0x80U) == 0) return false;

    const uint8_t count = status & 0x0FU;
    const uint8_t clear = 0;
    if (count == 0 || count > kMaxPoints) {
        tx(kGt911PointRegister, &clear, sizeof(clear));
        return false;
    }

    uint8_t raw[kMaxPoints * 8] = {};
    if (!rx(kGt911PointRegister + 1, raw, count * 8)) return false;
    if (!tx(kGt911PointRegister, &clear, sizeof(clear))) return false;

    point.count = count;
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t *entry = raw + (i * 8);
        // GT911 point records are track ID, X low/high, Y low/high,
        // strength low/high, reserved.
        point.x[i] = static_cast<uint16_t>(entry[2] << 8) | entry[1];
        point.y[i] = static_cast<uint16_t>(entry[4] << 8) | entry[3];
    }
    return true;
}
