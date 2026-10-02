#ifndef BOARD_HAS_PSRAM
#error "This example requires PSRAM. Enable PSRAM in the Arduino Tools menu."
#endif

#include <Arduino_GFX_Library.h>

#include "displays_config.h"

static Arduino_GFX *gfx = lcd43_create();

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  delay(1000);

  Serial.println("Arduino_GFX ASCII table example");
  if (gfx == nullptr || !gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    return;
  }

  const int16_t num_cols = (gfx->width() / 12) - 1;
  const int16_t num_rows = gfx->height() / 16;

  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_GREEN);
  for (int16_t x = 0; x < num_cols; ++x) {
    gfx->setCursor(16 + x * 12, 4);
    gfx->print(x);
  }

  gfx->setTextColor(RGB565_BLUE);
  for (int16_t y = 0; y < num_rows; ++y) {
    gfx->setCursor(4, 16 + y * 16);
    gfx->print(y);
  }

  char character = 0;
  for (int16_t y = 0; y < num_rows; ++y) {
    for (int16_t x = 0; x < num_cols; ++x) {
      gfx->drawChar(16 + x * 12, 16 + y * 16, character++, RGB565_WHITE, RGB565_BLACK);
    }
  }
}

void loop() {
  delay(1000);
}
