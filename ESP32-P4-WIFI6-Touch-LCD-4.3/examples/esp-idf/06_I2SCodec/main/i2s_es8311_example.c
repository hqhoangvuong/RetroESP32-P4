/*
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "example_config.h"

static const char *TAG = "i2s_codec";
static esp_codec_dev_handle_t speaker_codec;
static bool speaker_open;

#if CONFIG_EXAMPLE_MODE_ECHO
static esp_codec_dev_handle_t microphone_codec;
static bool microphone_open;
#endif

#if CONFIG_EXAMPLE_MODE_MUSIC
extern const uint8_t music_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t music_pcm_end[] asm("_binary_canon_pcm_end");
#endif

static esp_codec_dev_sample_info_t sample_info = {
    .bits_per_sample = 16,
    .channel = 2,
    .channel_mask = 0x03,
    .sample_rate = EXAMPLE_SAMPLE_RATE,
    .mclk_multiple = EXAMPLE_MCLK_MULTIPLE,
};

static void codec_devices_cleanup(void)
{
#if CONFIG_EXAMPLE_MODE_ECHO
    if (microphone_open) {
        int ret = esp_codec_dev_close(microphone_codec);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Failed to close ES7210 microphone codec: %d", ret);
        }
        microphone_open = false;
    }
    if (microphone_codec != NULL) {
        esp_codec_dev_delete(microphone_codec);
        microphone_codec = NULL;
    }
#endif

    if (speaker_open) {
        int ret = esp_codec_dev_close(speaker_codec);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "Failed to close ES8311 speaker codec: %d", ret);
        }
        speaker_open = false;
    }
    if (speaker_codec != NULL) {
        esp_codec_dev_delete(speaker_codec);
        speaker_codec = NULL;
    }
}

static esp_err_t codec_devices_init(void)
{
    speaker_codec = bsp_audio_codec_speaker_init();
    if (speaker_codec == NULL) {
        ESP_LOGE(TAG, "Failed to initialize the ES8311 speaker codec");
        return ESP_FAIL;
    }

    int codec_ret = esp_codec_dev_open(speaker_codec, &sample_info);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open the ES8311 speaker codec: %d", codec_ret);
        goto fail;
    }
    speaker_open = true;

    codec_ret = esp_codec_dev_set_out_vol(speaker_codec, EXAMPLE_VOICE_VOLUME);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set the ES8311 speaker volume: %d", codec_ret);
        goto fail;
    }

#if CONFIG_EXAMPLE_MODE_ECHO
    microphone_codec = bsp_audio_codec_microphone_init();
    if (microphone_codec == NULL) {
        ESP_LOGE(TAG, "Failed to initialize the ES7210 microphone codec");
        goto fail;
    }

    codec_ret = esp_codec_dev_open(microphone_codec, &sample_info);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open the ES7210 microphone codec: %d", codec_ret);
        goto fail;
    }
    microphone_open = true;

    codec_ret = esp_codec_dev_set_in_gain(microphone_codec, EXAMPLE_MIC_GAIN_DB);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to set the ES7210 microphone gain: %d", codec_ret);
        goto fail;
    }
#endif

    return ESP_OK;

fail:
    codec_devices_cleanup();
    return ESP_FAIL;
}

#if CONFIG_EXAMPLE_MODE_MUSIC
static void codec_music_task(void *args)
{
    (void)args;
    uint8_t *buffer = malloc(EXAMPLE_AUDIO_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Not enough memory for the music buffer");
        codec_devices_cleanup();
        vTaskDelete(NULL);
        return;
    }

    const size_t music_size = (size_t)(music_pcm_end - music_pcm_start);
    ESP_LOGI(TAG, "Music playback started (%u bytes)", (unsigned)music_size);

    while (true) {
        size_t offset = 0;
        while (offset < music_size) {
            size_t chunk_size = music_size - offset;
            if (chunk_size > EXAMPLE_AUDIO_BUFFER_SIZE) {
                chunk_size = EXAMPLE_AUDIO_BUFFER_SIZE;
            }
            memcpy(buffer, music_pcm_start + offset, chunk_size);
            int ret = esp_codec_dev_write(speaker_codec, buffer, (int)chunk_size);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "ES8311 playback failed at offset %u: %d", (unsigned)offset, ret);
                goto exit;
            }
            offset += chunk_size;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

exit:
    free(buffer);
    codec_devices_cleanup();
    vTaskDelete(NULL);
}
#else
static void codec_echo_task(void *args)
{
    (void)args;
    uint8_t *buffer = malloc(EXAMPLE_AUDIO_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Not enough memory for the echo buffer");
        codec_devices_cleanup();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ES7210-to-ES8311 microphone loopback started");
    while (true) {
        int ret = esp_codec_dev_read(microphone_codec, buffer, EXAMPLE_AUDIO_BUFFER_SIZE);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES7210 capture failed: %d", ret);
            break;
        }
        ret = esp_codec_dev_write(speaker_codec, buffer, EXAMPLE_AUDIO_BUFFER_SIZE);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 playback failed: %d", ret);
            break;
        }
    }

    free(buffer);
    codec_devices_cleanup();
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    printf("i2s codec example start\n-----------------------------\n");
    if (codec_devices_init() != ESP_OK) {
        ESP_LOGE(TAG, "Board audio codec initialization failed");
        return;
    }
#if CONFIG_EXAMPLE_MODE_MUSIC
    BaseType_t task_created = xTaskCreate(codec_music_task, "codec_music", 4096, NULL, 5, NULL);
#else
    BaseType_t task_created = xTaskCreate(codec_echo_task, "codec_echo", 8192, NULL, 5, NULL);
#endif
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the audio task");
        codec_devices_cleanup();
        return;
    }
    ESP_LOGI(TAG, "Board audio codec initialization succeeded");
}
