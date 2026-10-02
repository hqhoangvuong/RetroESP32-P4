/*
 * ST7701 LCD wrapper - ported from Arduino C++ to ESP-IDF C
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include <string.h>

#include "esp_lcd_st7701.h"
#include "st7701_lcd.h"
#include "ppa_engine.h"

#define LCD_H_RES 480
#define LCD_V_RES 800

#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)
#define LCD_BIT_PER_PIXEL (16)

#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

#ifdef CONFIG_BOARD_WAVESHARE_P4_43
/* Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3: LCD_RST=GPIO27, LCD_BL_PWM=GPIO26 (active low).
 * Keep in sync with components/odroid/include/pins_config.h. */
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL           0
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL          1
#define EXAMPLE_PIN_NUM_BK_LIGHT                GPIO_NUM_26
#define EXAMPLE_PIN_NUM_LCD_RST                 GPIO_NUM_27
#else
/* Guition ESP32-P4 4.3" LCD */
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL           1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL          0
#define EXAMPLE_PIN_NUM_BK_LIGHT                GPIO_NUM_23
#define EXAMPLE_PIN_NUM_LCD_RST                 GPIO_NUM_5
#endif

static const char *TAG = "st7701_lcd";

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

#ifdef CONFIG_BOARD_WAVESHARE_P4_43
/* ST7701 vendor init for the Waveshare panel (from the board BSP).  The driver
 * already sends the BK0 select, MADCTL (RGB) and COLMOD (RGB565) preamble, so
 * those three leading commands are not repeated here. */
static const st7701_lcd_init_cmd_t s_waveshare_init_cmds[] = {
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t []){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t []){0x10}, 1, 0},
    {0xB0, (uint8_t []){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09, 0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},
    {0xB1, (uint8_t []){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08, 0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x5D}, 1, 0},
    {0xB1, (uint8_t []){0x4E}, 1, 0},
    {0xB2, (uint8_t []){0x87}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4E}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xB9, (uint8_t []){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t []){0x03}, 1, 0},
    {0xBC, (uint8_t []){0x00}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xD0, (uint8_t []){0x88}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t []){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t []){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t []){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t []){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    {0x29, (uint8_t []){0x00}, 1, 20},
};
#endif /* CONFIG_BOARD_WAVESHARE_P4_43 */

static void enable_dsi_phy_power(void)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
}

static void init_lcd_backlight(void)
{
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL);
}

static void set_lcd_backlight(uint32_t level)
{
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, level);
}

esp_err_t st7701_lcd_init(void)
{
    enable_dsi_phy_power();
    init_lcd_backlight();

    // Create MIPI DSI bus
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_dbi_io_config_t dbi_config = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &s_io_handle));

    // Create DPI panel config
    esp_lcd_dpi_panel_config_t dpi_config = ST7701_480_800_PANEL_60HZ_DPI_CONFIG(MIPI_DPI_PX_FORMAT);
#ifdef CONFIG_BOARD_WAVESHARE_P4_43
    /* Waveshare panel timing: 30 MHz DPI clock, 500 Mbps/lane (same lane rate as default) */
    dpi_config.dpi_clock_freq_mhz = 30;
    dpi_config.video_timing.vsync_pulse_width = 8;
    dpi_config.video_timing.vsync_back_porch = 2;
    dpi_config.video_timing.vsync_front_porch = 60;
#endif

    st7701_vendor_config_t vendor_config = {
#ifdef CONFIG_BOARD_WAVESHARE_P4_43
        .init_cmds = s_waveshare_init_cmds,
        .init_cmds_size = sizeof(s_waveshare_init_cmds) / sizeof(s_waveshare_init_cmds[0]),
#endif
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags = {
            .use_mipi_interface = 1,
        }
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(s_io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    // Clear DPI framebuffer to black BEFORE turning on backlight
    // to avoid a white flash from uninitialized DPI buffer
    st7701_lcd_fill_screen(0x0000);

    // Turn on backlight
    set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);

    ESP_LOGI(TAG, "LCD initialized successfully (%dx%d, RGB565)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_err_t st7701_lcd_draw_bitmap(uint16_t x_start, uint16_t y_start,
                                  uint16_t x_end, uint16_t y_end,
                                  const uint16_t *color_data)
{
    return esp_lcd_panel_draw_bitmap(s_panel_handle, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t st7701_lcd_draw_rgb_bitmap(uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h,
                                      const uint16_t *color_data)
{
    return esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, color_data);
}

esp_err_t st7701_lcd_draw_to_fb(uint16_t x, uint16_t y,
                                uint16_t w, uint16_t h,
                                const uint16_t *data)
{
    void *fb = NULL;
    esp_err_t ret = esp_lcd_dpi_panel_get_frame_buffer(s_panel_handle, 1, &fb);
    if (ret != ESP_OK || !fb) return ret ? ret : ESP_ERR_INVALID_STATE;

    uint16_t *fb16 = (uint16_t *)fb;
    for (int row = 0; row < h; row++) {
        memcpy(&fb16[(y + row) * LCD_H_RES + x],
               &data[row * w], w * sizeof(uint16_t));
    }

    /* Flush the modified framebuffer rows so the DPI controller sees them */
    void *sync_start = &fb16[y * LCD_H_RES];
    size_t sync_size = (size_t)h * LCD_H_RES * sizeof(uint16_t);
    esp_cache_msync(sync_start, sync_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    return ESP_OK;
}

esp_err_t st7701_lcd_fill_screen(uint16_t color)
{
    void *color_data = NULL;
    size_t buf_size = 0;

    /* Use PPA hardware fill — much faster than CPU loop */
    esp_err_t ret = ppa_create_filled_buffer(LCD_H_RES, LCD_V_RES, color, &color_data, &buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA fill failed, falling back to CPU fill");
        buf_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
        uint16_t *cpu_buf = (uint16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!cpu_buf) return ESP_ERR_NO_MEM;
        for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) cpu_buf[i] = color;
        ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, cpu_buf);
        heap_caps_free(cpu_buf);
        return ret;
    }

    ret = esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, color_data);
    heap_caps_free(color_data);
    return ret;
}

void st7701_lcd_get_handles(bsp_lcd_handles_t *ret_handles)
{
    if (ret_handles) {
        ret_handles->io = s_io_handle;
        ret_handles->mipi_dsi_bus = NULL;
        ret_handles->panel = s_panel_handle;
        ret_handles->control = NULL;
    }
}

uint16_t st7701_lcd_width(void)
{
    return LCD_H_RES;
}

uint16_t st7701_lcd_height(void)
{
    return LCD_V_RES;
}
