/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Waveshare Electronics
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thin Arduino facade over the Espressif ESP LCD panel-I/O transport used by
 * esp_lcd_touch_gt911. GT911 is polled; the board does not drive INT or RST.
 */
#pragma once

#include <Arduino.h>

struct Gt911Point { uint16_t x[5]; uint16_t y[5]; uint8_t count; };

bool gt911_begin();
bool gt911_read(Gt911Point &point);
