#ifndef BOARD_HAS_PSRAM
#error "This example requires PSRAM. Enable PSRAM in the Arduino Tools menu."
#endif

#include <Arduino_GFX_Library.h>

#include "displays_config.h"
#include "gt911.h"

static constexpr uint8_t kMaxTouchPoints = 5;
static Arduino_GFX *gfx = lcd43_create();
static bool display_ready = false;
static bool touch_ready = false;

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  delay(1000);

  Serial.println("GT911 drawing board example");
  if (gfx == nullptr || !gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    return;
  }
  display_ready = true;

  // The shared GT911 driver polls the controller and probes 0x5D, then 0x14.
  // This board intentionally does not configure GT911 INT or RST pins.
  touch_ready = gt911_begin();
  if (!touch_ready) {
    Serial.println("GT911 unavailable; drawing is disabled");
  }

  gfx->fillScreen(RGB565_WHITE);
}

void loop() {
  if (!display_ready || !touch_ready) {
    delay(100);
    return;
  }

  Gt911Point points{};
  if (gt911_read(points)) {
    for (uint8_t i = 0; i < points.count && i < kMaxTouchPoints; ++i) {
      if (points.x[i] < gfx->width() && points.y[i] < gfx->height()) {
        gfx->fillCircle(points.x[i], points.y[i], 5, RGB565_BLUE);
      }
    }
  }
  // The controller is polling-only on this board. Keep reads at least 20 ms
  // apart to match the product's ESP-IDF touch polling requirement.
  delay(20);
}
