/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"
#include "app_usb.h"
#include "usb_descriptors.h"

static const char *TAG = "app_touch";

static esp_lcd_touch_handle_t tp = NULL;

static void app_touch_task(void *arg)
{
    (void)arg;

    uint16_t x[USB_HID_TOUCH_MAX_POINTS];
    uint16_t y[USB_HID_TOUCH_MAX_POINTS];
    uint16_t strength[USB_HID_TOUCH_MAX_POINTS];
    uint8_t touchpad_cnt = 0;
    bool send_press = false;
    while (1) {
        esp_err_t ret = esp_lcd_touch_read_data(tp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool touchpad_pressed = esp_lcd_touch_get_coordinates(tp, x, y, strength, &touchpad_cnt,
                                                              USB_HID_TOUCH_MAX_POINTS);
        hid_report_t report = {0};
        if (touchpad_pressed && touchpad_cnt > 0) {
            report.report_id = REPORT_ID_TOUCH;
            for (int i = 0; i < touchpad_cnt; i++) {
                report.touch_report.data[i].index = 0;
                report.touch_report.data[i].press_down = 1;
                report.touch_report.data[i].x = x[i];
                report.touch_report.data[i].y = y[i];
                report.touch_report.data[i].width = strength[i];
                report.touch_report.data[i].height = strength[i];
            }
            ESP_LOGD(TAG, "touchpad cnt: %d\n", touchpad_cnt);
            report.touch_report.cnt = touchpad_cnt;
#if CFG_TUD_HID
            ret = tinyusb_hid_keyboard_report(report);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Touch report enqueue failed: %s", esp_err_to_name(ret));
            }
#endif
            send_press = true;
        } else if (send_press) {
            send_press = false;
            report.report_id = REPORT_ID_TOUCH;
#if CFG_TUD_HID
            ret = tinyusb_hid_keyboard_report(report);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Touch release enqueue failed: %s", esp_err_to_name(ret));
            }
#endif
            ESP_LOGD(TAG, "send release %d", touchpad_cnt);
        }
            
        // Reading from the GT911 at a time shorter than this may result in false reports.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t app_touch_init(void)
{
    bsp_display_cfg_t cfg = {
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(bsp_touch_new(&cfg, &tp), TAG, "Touch initialization failed");

    BaseType_t task_created = xTaskCreate(app_touch_task, "app_touch_task", 4096, NULL,
                                          CONFIG_TOUCH_TASK_PRIORITY, NULL);
    if (task_created != pdPASS) {
        esp_lcd_touch_del(tp);
        tp = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
