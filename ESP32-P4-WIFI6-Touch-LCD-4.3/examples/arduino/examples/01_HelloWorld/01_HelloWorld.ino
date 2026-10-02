#ifndef BOARD_HAS_PSRAM
#error "This example requires PSRAM. Enable PSRAM in the Arduino Tools menu."
#endif

#include <Arduino_GFX_Library.h>

#include "displays_config.h"

static Arduino_GFX *gfx = lcd43_create();
static bool display_ready = false;

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  delay(1000);

  Serial.println("Arduino_GFX Hello World example");
  if (gfx == nullptr || !gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    return;
  }
  display_ready = true;
  Serial.println("gfx->begin() ok");

  // These bars make it easy to confirm that the DSI panel and framebuffer
  // work before the text demo starts.
  gfx->fillScreen(RGB565_RED);
  delay(1500);
  gfx->fillScreen(RGB565_GREEN);
  delay(1500);
  gfx->fillScreen(RGB565_BLUE);
  delay(1500);

  gfx->fillScreen(RGB565_BLACK);
  gfx->setCursor(10, 10);
  gfx->setTextColor(RGB565_WHITE);
  gfx->println("Hello World!");
  Serial.println("HelloWorld on screen");
}

void loop() {
  if (!display_ready) {
    delay(1000);
    return;
  }

  gfx->setCursor(random(gfx->width()), random(gfx->height()));
  gfx->setTextColor(random(0xffff), random(0xffff));
  gfx->setTextSize(random(1, 6), random(1, 6), random(2));
  gfx->println("Hello World!");
  delay(1000);
}
