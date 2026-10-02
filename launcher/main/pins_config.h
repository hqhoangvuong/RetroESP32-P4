#pragma once

#include "sdkconfig.h"

/* Board selection: CONFIG_BOARD_WAVESHARE_P4_43 (see odroid/Kconfig.projbuild)
 * picks the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 pin map; otherwise the
 * Guition ESP32-P4 4.3" LCD board is assumed.  LCD reset, ST7701 init and DPI
 * timing live in components/st7701_lcd/st7701_lcd.c.
 *
 * NOTE: components/app_common/include/pins_config.h and
 * launcher/main/pins_config.h are verbatim copies — keep them in sync. */

/* LCD Resolution */
#define LCD_H_RES 480
#define LCD_V_RES 800

/* LCD Reset Pin */
#define LCD_RST   -1

#ifdef CONFIG_BOARD_WAVESHARE_P4_43
/* LCD Backlight: LCD_BL_PWM on GPIO26, active-LOW (LEDC output inverted) */
#define LCD_BK_LIGHT_GPIO       26
#define LCD_BK_LIGHT_ACTIVE_LOW 1
#else
/* LCD Backlight: GPIO23, active-high */
#define LCD_BK_LIGHT_GPIO       23
#define LCD_BK_LIGHT_ACTIVE_LOW 0
#endif

/* Touch I2C Pins (GT911; INT/RST not driven on either board) */
#define TP_I2C_SDA  7
#define TP_I2C_SCL  8
#define TP_RST      -1
#define TP_INT      -1

/* I2S / ES8311 Audio Codec Pins */
#define I2S_MCLK_IO   13
#define I2S_BCLK_IO   12
#define I2S_WS_IO     10
#define I2S_DOUT_IO    9   /* ESP32 -> ES8311 SDIN */
#ifdef CONFIG_BOARD_WAVESHARE_P4_43
#define I2S_DIN_IO    11   /* ES7210/ES8311 DOUT -> ESP32 */
#define AUDIO_PA_IO   53   /* NS4150B CTRL (active high) */
#else
#define I2S_DIN_IO    48   /* ES8311 DOUT -> ESP32 */
#define AUDIO_PA_IO   11   /* Power Amplifier enable (active high) */
#endif

/* SD MMC Pins (identical on both boards) */
#define SD_MMC_CLK  43
#define SD_MMC_CMD  44
#define SD_MMC_D0   39
#define SD_MMC_D1   40
#define SD_MMC_D2   41
#define SD_MMC_D3   42

/* Battery sense ADC.  Vgpio = Vbat * DEN / NUM */
#ifdef CONFIG_BOARD_WAVESHARE_P4_43
#define BATTERY_ADC_GPIO     20    /* BAT_ADC, ADC1 */
#define BATTERY_DIVIDER_NUM  300   /* 200K (R12) + 100K (R15) */
#define BATTERY_DIVIDER_DEN  100
#else
#define BATTERY_ADC_GPIO     53    /* ADC2_CH4 */
#define BATTERY_DIVIDER_NUM  168   /* 68K + 100K */
#define BATTERY_DIVIDER_DEN  100
#endif
