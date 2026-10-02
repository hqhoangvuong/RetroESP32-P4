/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "usb_descriptors.h"
#include "device/usbd.h"
#include "app_usb.h"

static const char *TAG = "app_usb";
static usb_phy_handle_t s_phy_handle = NULL;

//--------------------------------------------------------------------+
// USB PHY config
//--------------------------------------------------------------------+
static esp_err_t usb_phy_init(void)
{
    if (s_phy_handle) {
        return ESP_OK;
    }

    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    phy_conf.target = USB_PHY_TARGET_INT;
    return usb_new_phy(&phy_conf, &s_phy_handle);
}

static void tusb_device_task(void *arg)
{
    (void)arg;

    while (1) {
        tud_task();
    }
}

esp_err_t app_usb_init(void)
{
    ESP_RETURN_ON_ERROR(usb_phy_init(), TAG, "USB PHY initialization failed");
    bool usb_init = tusb_init();
    if (!usb_init) {
        ESP_LOGE(TAG, "USB Device Stack Init Fail");
        return ESP_FAIL;
    }

#if CFG_TUD_VENDOR
    ESP_RETURN_ON_ERROR(app_vendor_init(), TAG, "app_vendor_init failed");
#endif

#if CFG_TUD_HID
    ESP_RETURN_ON_ERROR(app_hid_init(), TAG, "app_hid_init failed");
#endif

#if CFG_TUD_AUDIO
    ESP_RETURN_ON_ERROR(app_uac_init(), TAG, "app_uac_init failed");
#endif

    BaseType_t task_created = xTaskCreate(tusb_device_task, "tusb_device_task", 4096, NULL,
                                          CONFIG_USB_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_ERR_NO_MEM, TAG, "USB task creation failed");
    return ESP_OK;
}

/************************************************** TinyUSB callbacks ***********************************************/
// Invoked when device is mounted
void tud_mount_cb(void)
{
#if CFG_TUD_VENDOR
    // BUS_RESET can start a new host session without an umount callback.
    app_vendor_reset();
#endif
    ESP_LOGI(TAG, "USB Mount");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
#if CFG_TUD_VENDOR
    app_vendor_reset();
#endif
    ESP_LOGI(TAG, "USB Un-Mount");
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void) remote_wakeup_en;
    ESP_LOGI(TAG, "USB Suspend");
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "USB Resume");
}
