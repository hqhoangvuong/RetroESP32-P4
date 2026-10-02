/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_ldo_regulator.h"   
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_cache.h"
#include "esp_dma_utils.h"
#include "esp_private/esp_cache_private.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/gpio.h"
#include "driver/jpeg_decode.h"
#include "app_lcd.h"
#include "app_usb.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "sdkconfig.h"

static const char *TAG = "app_lcd";

static esp_lcd_panel_handle_t display_handle;
static jpeg_decoder_handle_t jpgd_handle = NULL;

static jpeg_decode_cfg_t decode_cfg = {
#if CONFIG_LCD_PIXEL_FORMAT_RGB565
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
#elif CONFIG_LCD_PIXEL_FORMAT_RGB888
    .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
#else
#error "Select a supported LCD pixel format"
#endif
    .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
};

#if (defined(CONFIG_LCD_PIXEL_FORMAT_RGB565) && !defined(CONFIG_BSP_LCD_COLOR_FORMAT_RGB565)) || \
    (defined(CONFIG_LCD_PIXEL_FORMAT_RGB888) && !defined(CONFIG_BSP_LCD_COLOR_FORMAT_RGB888))
#error "The application JPEG output format must match the BSP LCD color format"
#endif

static uint32_t out_size = 0;
static void *lcd_buffer[EXAMPLE_LCD_BUF_NUM];
static uint8_t buf_index = 0;
static bool init_ok = false;

static bool jpeg_marker_is_start_of_frame(uint8_t marker)
{
    return (marker >= 0xc0 && marker <= 0xc3) ||
           (marker >= 0xc5 && marker <= 0xc7) ||
           (marker >= 0xc9 && marker <= 0xcb) ||
           (marker >= 0xcd && marker <= 0xcf);
}

static esp_err_t app_lcd_validate_jpeg(const uint8_t *data, size_t length)
{
    if (data == NULL || length < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return ESP_ERR_INVALID_ARG;
    }

    bool found_sof0 = false;
    size_t offset = 2;
    while (offset < length) {
        if (data[offset] != 0xff) {
            return ESP_ERR_INVALID_ARG;
        }
        while (offset < length && data[offset] == 0xff) {
            offset++;
        }
        if (offset >= length) {
            return ESP_ERR_INVALID_ARG;
        }

        uint8_t marker = data[offset++];
        if (marker == 0x00 || marker == 0x01 || marker == 0xd8 || marker == 0xd9 ||
                (marker >= 0xd0 && marker <= 0xd7)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (length - offset < 2) {
            return ESP_ERR_INVALID_ARG;
        }

        size_t segment_length = ((size_t)data[offset] << 8) | data[offset + 1];
        if (segment_length < 2 || segment_length > length - offset) {
            return ESP_ERR_INVALID_ARG;
        }

        if (marker == 0xda) {
            if (!found_sof0 || segment_length < 6) {
                return ESP_ERR_INVALID_ARG;
            }
            uint8_t component_count = data[offset + 2];
            if (component_count == 0 || segment_length != 6U + 2U * component_count) {
                return ESP_ERR_INVALID_ARG;
            }
            return ESP_OK;
        }

        if (jpeg_marker_is_start_of_frame(marker)) {
            if (marker != 0xc0 || found_sof0 || segment_length < 8) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            uint8_t component_count = data[offset + 7];
            if (data[offset + 2] != 8 || component_count == 0 ||
                    segment_length != 8U + 3U * component_count) {
                return ESP_ERR_INVALID_ARG;
            }
            uint16_t height = ((uint16_t)data[offset + 3] << 8) | data[offset + 4];
            uint16_t width = ((uint16_t)data[offset + 5] << 8) | data[offset + 6];
            if (width != EXAMPLE_LCD_H_RES || height != EXAMPLE_LCD_V_RES) {
                return ESP_ERR_INVALID_SIZE;
            }
            found_sof0 = true;
        }

        offset += segment_length;
    }

    return ESP_ERR_INVALID_ARG;
}

static void app_lcd_delete_display(bsp_lcd_handles_t *handles)
{
    if (handles->panel) {
        esp_err_t err = esp_lcd_panel_del(handles->panel);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete LCD panel: %s", esp_err_to_name(err));
        }
        handles->panel = NULL;
    }
    if (handles->control) {
        esp_err_t err = esp_lcd_panel_del(handles->control);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete LCD control panel: %s", esp_err_to_name(err));
        }
        handles->control = NULL;
    }
    if (handles->io) {
        esp_err_t err = esp_lcd_panel_io_del(handles->io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete LCD panel IO: %s", esp_err_to_name(err));
        }
        handles->io = NULL;
    }
    if (handles->mipi_dsi_bus) {
        esp_err_t err = esp_lcd_del_dsi_bus(handles->mipi_dsi_bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete MIPI DSI bus: %s", esp_err_to_name(err));
        }
        handles->mipi_dsi_bus = NULL;
    }
}

esp_err_t app_lcd_draw(uint8_t *buf, uint32_t len, uint16_t width, uint16_t height)
{
    static int fps_count = 0;
    static int64_t start_time = 0;
    fps_count++;
    if (fps_count == 50) {
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
        start_time = end_time;
        fps_count = 0;
    }
    ESP_RETURN_ON_FALSE(init_ok && jpgd_handle && display_handle, ESP_ERR_INVALID_STATE, TAG,
                        "LCD is not initialized");
    ESP_RETURN_ON_FALSE(buf && len, ESP_ERR_INVALID_ARG, TAG, "Invalid JPEG input");
    ESP_RETURN_ON_FALSE(width == EXAMPLE_LCD_H_RES && height == EXAMPLE_LCD_V_RES,
                        ESP_ERR_INVALID_SIZE, TAG, "Unexpected frame size: %" PRIu16 "x%" PRIu16,
                        width, height);

    esp_err_t ret = app_lcd_validate_jpeg(buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Unsafe or unsupported JPEG marker stream: %s", esp_err_to_name(ret));
        return ret;
    }

    jpeg_decode_picture_info_t picture_info = {0};
    ret = jpeg_decoder_get_info(buf, len, &picture_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Invalid JPEG header: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_RETURN_ON_FALSE(picture_info.width == EXAMPLE_LCD_H_RES &&
                        picture_info.height == EXAMPLE_LCD_V_RES,
                        ESP_ERR_INVALID_SIZE, TAG, "Unexpected JPEG size: %" PRIu32 "x%" PRIu32,
                        picture_info.width, picture_info.height);

    ret = jpeg_decoder_process(jpgd_handle, &decode_cfg, buf, len, lcd_buffer[buf_index],
                               EXAMPLE_LCD_BUF_LEN, &out_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_RETURN_ON_FALSE(out_size == EXAMPLE_LCD_BUF_LEN, ESP_ERR_INVALID_SIZE, TAG,
                        "Unexpected decoded frame size: %" PRIu32, out_size);

    ret = esp_lcd_panel_draw_bitmap(display_handle, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
                                    lcd_buffer[buf_index]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD draw failed: %s", esp_err_to_name(ret));
        return ret;
    }

    buf_index = (buf_index + 1) == EXAMPLE_LCD_BUF_NUM ? 0 : (buf_index + 1);
    return ESP_OK;
}


esp_err_t app_lcd_init(void)
{
    if (init_ok) {
        return ESP_OK;
    }

    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .intr_priority = 1,
        .timeout_ms = 50,
    };

    esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &jpgd_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "JPEG decoder initialization failed");

    bsp_display_config_t disp_config = {0};
    bsp_lcd_handles_t display_handles = {0};

    ESP_LOGI(TAG, "Initialize MIPI DSI bus p4");

    ret = bsp_display_new_with_handles(&disp_config, &display_handles);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Display initialization failed: %s", esp_err_to_name(ret));
        goto fail;
    }

#if EXAMPLE_LCD_BUF_NUM == 1
    ret = esp_lcd_dpi_panel_get_frame_buffer(display_handles.panel, 1, &lcd_buffer[0]);
#elif EXAMPLE_LCD_BUF_NUM == 2
    ret = esp_lcd_dpi_panel_get_frame_buffer(display_handles.panel, 2, &lcd_buffer[0], &lcd_buffer[1]);
#else
    ret = esp_lcd_dpi_panel_get_frame_buffer(display_handles.panel, 3, &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]);
#endif
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get LCD frame buffers: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = bsp_display_backlight_on();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to turn on display backlight: %s", esp_err_to_name(ret));
        goto fail;
    }

    display_handle = display_handles.panel;
    init_ok = true;
    return ESP_OK;

fail:
    app_lcd_delete_display(&display_handles);
    memset(lcd_buffer, 0, sizeof(lcd_buffer));
    display_handle = NULL;
    if (jpgd_handle) {
        esp_err_t cleanup_ret = jpeg_del_decoder_engine(jpgd_handle);
        if (cleanup_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete JPEG decoder: %s", esp_err_to_name(cleanup_ret));
        }
        jpgd_handle = NULL;
    }
    return ret;
}
