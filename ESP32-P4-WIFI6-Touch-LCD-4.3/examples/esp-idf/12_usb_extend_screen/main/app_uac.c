/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_usb.h"
#include "esp_log.h"
#include "usb_device_uac.h"
#include "bsp/esp-bsp.h"
#include "usb_descriptors.h"

#include "bsp_board_extra.h"


static const char *TAG = "app_uac";

static esp_err_t uac_device_output_cb(uint8_t *buf, size_t len, void *arg)
{
    (void)arg;
    size_t bytes_written = 0;
    esp_err_t ret = bsp_extra_i2s_write(buf, len, &bytes_written, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t uac_device_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *arg)
{
    (void)arg;
    esp_err_t ret = bsp_extra_i2s_read(buf, len, bytes_read, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void uac_device_set_mute_cb(uint32_t mute, void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "uac_device_set_mute_cb: %"PRIu32"", mute);
    esp_err_t ret = bsp_extra_codec_mute_set(mute != 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set mute failed: %s", esp_err_to_name(ret));
    }
}

static void uac_device_set_volume_cb(uint32_t volume, void *arg)
{
    (void)arg;
    ESP_LOGD(TAG, "uac_device_set_volume_cb: %"PRIu32"", volume);
    int volume_set = 0;
    esp_err_t ret = bsp_extra_codec_volume_set((int)volume, &volume_set);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set volume failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t app_uac_init(void)
{
    esp_err_t ret = bsp_extra_codec_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bsp_extra_codec_set_fs(CONFIG_UAC_SAMPLE_RATE, 16, CONFIG_UAC_SPEAKER_CHANNEL_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec format configuration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uac_device_config_t config = {
        .skip_tinyusb_init = true,
        .output_cb = uac_device_output_cb,
        .input_cb = uac_device_input_cb,
        .set_mute_cb = uac_device_set_mute_cb,
        .set_volume_cb = uac_device_set_volume_cb,
        .cb_ctx = NULL,
        .spk_itf_num = ITF_NUM_AUDIO_STREAMING_SPK,
        .mic_itf_num = ITF_NUM_AUDIO_STREAMING_MIC,
    };

    ret = uac_device_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC device init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
