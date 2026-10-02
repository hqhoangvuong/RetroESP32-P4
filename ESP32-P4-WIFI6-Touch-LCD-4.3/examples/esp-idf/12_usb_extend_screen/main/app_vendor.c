/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_usb.h"
#include "app_lcd.h"
#include "esp_timer.h"
#include "usb_frame.h"

static const char *TAG = "app_vendor";

//--------------------------------------------------------------------+
// Vendor callbacks
//--------------------------------------------------------------------+

#define CONFIG_USB_VENDOR_RX_BUFSIZE  VENDOR_BUF_SIZE
#define UDISP_FRAME_HEADER_SIZE        16U
#define UDISP_FRAME_ID_MASK            0x3ffU

// -- Display Packets
#define UDISP_TYPE_JPG     3
#define UDISP_TYPE_END     0xff

typedef struct {
    uint8_t type;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t frame_id;
    uint32_t payload_total;
} udisp_frame_header_t;

typedef enum {
    RX_HEADER,
    RX_PAYLOAD,
    RX_DISCARD,
} rx_state_t;

static rx_state_t rx_state = RX_HEADER;
static uint8_t header_buffer[UDISP_FRAME_HEADER_SIZE];
static size_t header_received = 0;
static size_t payload_remaining = 0;
static frame_t *current_frame = NULL;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void parse_header(const uint8_t *data, udisp_frame_header_t *header)
{
    const uint32_t frame_info = read_le32(&data[12]);

    header->type = data[2];
    header->x = read_le16(&data[4]);
    header->y = read_le16(&data[6]);
    header->width = read_le16(&data[8]);
    header->height = read_le16(&data[10]);
    header->frame_id = frame_info & UDISP_FRAME_ID_MASK;
    header->payload_total = frame_info >> 10;
}

static void recycle_frame(frame_t *frame, const char *context)
{
    if (!frame) {
        return;
    }

    const esp_err_t ret = frame_return_empty(frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: failed to return frame: %s", context, esp_err_to_name(ret));
    }
}

static void count_input_frame(void)
{
    static unsigned int fps_count = 0;
    static int64_t start_time = 0;

    if (fps_count == 0) {
        start_time = esp_timer_get_time();
    }
    fps_count++;
    if (fps_count == 50) {
        const int64_t elapsed = esp_timer_get_time() - start_time;
        if (elapsed > 0) {
            ESP_LOGI(TAG, "Input fps: %.2f", 50000000.0 / (double)elapsed);
        }
        fps_count = 0;
    }
}

static void begin_payload(const udisp_frame_header_t *header)
{
    payload_remaining = header->payload_total;

    if (header->payload_total == 0) {
        if (header->type != UDISP_TYPE_END) {
            ESP_LOGW(TAG, "Dropping zero-length frame %" PRIu32, header->frame_id);
        }
        rx_state = RX_HEADER;
        return;
    }

    if (header->type != UDISP_TYPE_JPG || header->x != 0 || header->y != 0 ||
            header->width != EXAMPLE_LCD_H_RES || header->height != EXAMPLE_LCD_V_RES) {
        ESP_LOGW(TAG, "Dropping unsupported frame %" PRIu32
                 " (type=%u, x=%" PRIu16 ", y=%" PRIu16 ", size=%" PRIu16 "x%" PRIu16 ")",
                 header->frame_id, header->type, header->x, header->y,
                 header->width, header->height);
        rx_state = RX_DISCARD;
        return;
    }

    if (header->payload_total > CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B) {
        ESP_LOGW(TAG, "Frame %" PRIu32 " payload (%" PRIu32
                 " bytes) exceeds configured limit (%u bytes)",
                 header->frame_id, header->payload_total,
                 (unsigned int)CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
        rx_state = RX_DISCARD;
        return;
    }

    frame_t *frame = frame_get_empty();
    if (!frame) {
        ESP_LOGW(TAG, "No frame buffer available; dropping frame %" PRIu32, header->frame_id);
        rx_state = RX_DISCARD;
        return;
    }

    frame_reset(frame);
    if (!frame->data || (size_t)header->payload_total > frame->data_buffer_len) {
        ESP_LOGW(TAG, "Frame %" PRIu32 " payload (%" PRIu32
                 " bytes) exceeds buffer capacity (%zu bytes)",
                 header->frame_id, header->payload_total, frame->data_buffer_len);
        recycle_frame(frame, "oversized frame");
        rx_state = RX_DISCARD;
        return;
    }

    frame->info.width = header->width;
    frame->info.height = header->height;
    frame->info.total = header->payload_total;
    frame->info.received = 0;
    current_frame = frame;
    rx_state = RX_PAYLOAD;
    count_input_frame();
}

static bool is_complete_jpeg(const frame_t *frame)
{
    return frame && frame->data_len >= 4 &&
           frame->data[0] == 0xff && frame->data[1] == 0xd8 &&
           frame->data[frame->data_len - 2] == 0xff &&
           frame->data[frame->data_len - 1] == 0xd9;
}

static void finish_payload(void)
{
    frame_t *frame = current_frame;
    current_frame = NULL;
    payload_remaining = 0;
    rx_state = RX_HEADER;

    if (!frame) {
        return;
    }

    frame->info.received = (uint32_t)frame->data_len;
    if (frame->data_len != frame->info.total || !is_complete_jpeg(frame)) {
        ESP_LOGW(TAG, "Dropping invalid JPEG payload (%zu/%" PRIu32 " bytes)",
                 frame->data_len, frame->info.total);
        recycle_frame(frame, "invalid JPEG");
        return;
    }

    const esp_err_t ret = frame_send_filled(frame);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue completed frame: %s", esp_err_to_name(ret));
        recycle_frame(frame, "filled queue failure");
    }
}

static void process_rx_data(const uint8_t *data, size_t data_len)
{
    while (data_len > 0) {
        if (rx_state == RX_HEADER) {
            const size_t needed = UDISP_FRAME_HEADER_SIZE - header_received;
            const size_t copy_len = data_len < needed ? data_len : needed;
            memcpy(&header_buffer[header_received], data, copy_len);
            header_received += copy_len;
            data += copy_len;
            data_len -= copy_len;

            if (header_received == UDISP_FRAME_HEADER_SIZE) {
                udisp_frame_header_t header = {0};
                parse_header(header_buffer, &header);
                header_received = 0;
                begin_payload(&header);
            }
            continue;
        }

        if (rx_state == RX_DISCARD) {
            const size_t discard_len = data_len < payload_remaining ? data_len : payload_remaining;
            data += discard_len;
            data_len -= discard_len;
            payload_remaining -= discard_len;
            if (payload_remaining == 0) {
                rx_state = RX_HEADER;
            }
            continue;
        }

        if (!current_frame || payload_remaining == 0) {
            finish_payload();
            continue;
        }

        const size_t copy_len = data_len < payload_remaining ? data_len : payload_remaining;
        const esp_err_t ret = frame_add_data(current_frame, data, copy_len);
        data += copy_len;
        data_len -= copy_len;
        payload_remaining -= copy_len;

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to append frame payload: %s", esp_err_to_name(ret));
            frame_t *frame = current_frame;
            current_frame = NULL;
            recycle_frame(frame, "payload append failure");
            rx_state = payload_remaining == 0 ? RX_HEADER : RX_DISCARD;
            continue;
        }

        current_frame->info.received = (uint32_t)current_frame->data_len;
        if (payload_remaining == 0) {
            finish_payload();
        }
    }
}

void app_vendor_reset(void)
{
    frame_t *frame = current_frame;

    current_frame = NULL;
    header_received = 0;
    payload_remaining = 0;
    rx_state = RX_HEADER;
    recycle_frame(frame, "USB stream reset");
}

static void transfer_task(void *pv_parameter)
{
    (void)pv_parameter;

    while (true) {
        frame_t *frame = frame_get_filled();
        if (!frame) {
            ESP_LOGE(TAG, "Failed to receive completed frame");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        esp_err_t ret = ESP_ERR_INVALID_SIZE;
        if (frame->data_len <= UINT32_MAX) {
            ret = app_lcd_draw(frame->data, (uint32_t)frame->data_len,
                               frame->info.width, frame->info.height);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Frame decode/draw failed: %s", esp_err_to_name(ret));
        }

        ret = frame_return_empty(frame);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to recycle displayed frame: %s", esp_err_to_name(ret));
        }
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint16_t bufsize)
{
    static uint8_t rx_buf[CONFIG_USB_VENDOR_RX_BUFSIZE];

    (void)buffer;
    (void)bufsize;

    while (tud_vendor_n_available(itf)) {
        const int read_res = tud_vendor_n_read(itf, rx_buf, sizeof(rx_buf));
        if (read_res <= 0) {
            break;
        }
        process_rx_data(rx_buf, (size_t)read_res);
    }
}

esp_err_t app_vendor_init(void)
{
    esp_err_t ret = frame_allocate(6, CONFIG_USB_EXTEND_SCREEN_FRAME_LIMIT_B);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Frame buffer allocation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const BaseType_t task_created = xTaskCreatePinnedToCore(transfer_task, "transfer_task", 4096,
                                                            NULL, CONFIG_VENDOR_TASK_PRIORITY, NULL, 0);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Transfer task creation failed");
        frame_deallocate();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
