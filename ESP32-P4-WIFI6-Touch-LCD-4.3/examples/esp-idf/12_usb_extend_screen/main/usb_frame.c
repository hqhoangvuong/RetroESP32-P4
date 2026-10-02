/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb_frame.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/jpeg_decode.h"
#elif CONFIG_IDF_TARGET_ESP32S3
#include "esp_heap_caps.h"
#endif

static QueueHandle_t empty_fb_queue = NULL;
static QueueHandle_t filled_fb_queue = NULL;
static const char *TAG = "usb_frame";

static void frame_free(frame_t *frame)
{
    if (frame) {
        free(frame->data);
        free(frame);
    }
}

void frame_deallocate(void)
{
    frame_t *frame = NULL;

    if (empty_fb_queue) {
        while (xQueueReceive(empty_fb_queue, &frame, 0) == pdPASS) {
            frame_free(frame);
        }
        vQueueDelete(empty_fb_queue);
        empty_fb_queue = NULL;
    }

    if (filled_fb_queue) {
        while (xQueueReceive(filled_fb_queue, &frame, 0) == pdPASS) {
            frame_free(frame);
        }
        vQueueDelete(filled_fb_queue);
        filled_fb_queue = NULL;
    }
}

esp_err_t frame_allocate(int nb_of_fb, size_t fb_size)
{
    if (nb_of_fb <= 0 || fb_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (empty_fb_queue || filled_fb_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    // We will be passing the frame buffers by reference
    empty_fb_queue = xQueueCreate(nb_of_fb, sizeof(frame_t *));
    if (!empty_fb_queue) {
        ESP_LOGE(TAG, "Not enough memory for empty_fb_queue %d", nb_of_fb);
        return ESP_ERR_NO_MEM;
    }
    filled_fb_queue = xQueueCreate(nb_of_fb, sizeof(frame_t *));
    if (!filled_fb_queue) {
        ESP_LOGE(TAG, "Not enough memory for filled_fb_queue %d", nb_of_fb);
        frame_deallocate();
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < nb_of_fb; i++) {
        // Allocate the frame buffer
        frame_t *this_fb = calloc(1, sizeof(frame_t));
        if (!this_fb) {
            ESP_LOGE(TAG, "Not enough memory for frame metadata %d", i);
            frame_deallocate();
            return ESP_ERR_NO_MEM;
        }

        uint8_t *this_data = NULL;
        size_t allocated_size = fb_size;
#if CONFIG_IDF_TARGET_ESP32P4
        size_t malloc_size = 0;
        jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        this_data = (uint8_t *)jpeg_alloc_decoder_mem(fb_size, &tx_mem_cfg, &malloc_size);
        allocated_size = malloc_size;
#elif CONFIG_IDF_TARGET_ESP32S3
        this_data = (uint8_t *)heap_caps_aligned_alloc(16, fb_size, MALLOC_CAP_SPIRAM);
#else
        this_data = malloc(fb_size);
#endif
        if (!this_data || allocated_size < fb_size) {
            ESP_LOGE(TAG, "Not enough memory for frame buffer %d (%zu bytes)", i, fb_size);
            free(this_data);
            free(this_fb);
            frame_deallocate();
            return ESP_ERR_NO_MEM;
        }

        // Set members to default
        this_fb->data = this_data;
        this_fb->data_buffer_len = allocated_size;

        // Add the frame to Queue of empty frames
        const BaseType_t result = xQueueSend(empty_fb_queue, &this_fb, 0);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to queue frame buffer %d", i);
            frame_free(this_fb);
            frame_deallocate();
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void frame_reset(frame_t *frame)
{
    if (!frame) {
        return;
    }
    frame->data_len = 0;
    memset(&frame->info, 0, sizeof(frame->info));
}

esp_err_t frame_return_empty(frame_t *frame)
{
    ESP_RETURN_ON_FALSE(frame && empty_fb_queue, ESP_ERR_INVALID_ARG, TAG, "Invalid frame or empty queue");
    frame_reset(frame);
    BaseType_t result = xQueueSend(empty_fb_queue, &frame, 0);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_TIMEOUT, TAG, "empty_fb_queue is full");
    return ESP_OK;
}

esp_err_t frame_send_filled(frame_t *frame)
{
    ESP_RETURN_ON_FALSE(frame && filled_fb_queue && frame->data && frame->data_len > 0 &&
                        frame->data_len <= frame->data_buffer_len,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid filled frame");
    BaseType_t result = xQueueSend(filled_fb_queue, &frame, 0);
    ESP_RETURN_ON_FALSE(result == pdPASS, ESP_ERR_TIMEOUT, TAG, "filled_fb_queue is full");
    return ESP_OK;
}

esp_err_t frame_add_data(frame_t *frame, const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(frame && frame->data && data && data_len, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");
    if (frame->data_len > frame->data_buffer_len || data_len > frame->data_buffer_len - frame->data_len) {
        ESP_LOGD(TAG, "Frame buffer overflow");
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(frame->data + frame->data_len, data, data_len);
    frame->data_len += data_len;
    return ESP_OK;
}

frame_t *frame_get_empty(void)
{
    frame_t *this_fb = NULL;
    if (empty_fb_queue && xQueueReceive(empty_fb_queue, &this_fb, 0) == pdPASS) {
        return this_fb;
    }
    return NULL;
}

frame_t *frame_get_filled(void)
{
    frame_t *this_fb = NULL;
    if (filled_fb_queue && xQueueReceive(filled_fb_queue, &this_fb, portMAX_DELAY) == pdPASS) {
        return this_fb;
    }
    return NULL;
}
