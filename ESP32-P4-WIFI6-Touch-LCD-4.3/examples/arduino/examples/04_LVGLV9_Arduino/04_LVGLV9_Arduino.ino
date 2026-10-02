#ifndef BOARD_HAS_PSRAM
#error "This example requires PSRAM. Enable PSRAM in the Arduino Tools menu."
#endif

#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "displays_config.h"
#include "gt911.h"

static constexpr uint32_t kLvglTickPeriodMs = 5;
static constexpr uint16_t kDrawBufferHeight = 50;

static Arduino_GFX *gfx = lcd43_create();
static lv_display_t *lv_display = nullptr;
static lv_indev_t *touchpad = nullptr;
static lv_color_t *draw_buffer_1 = nullptr;
static lv_color_t *draw_buffer_2 = nullptr;
static bool touch_ready = false;
static uint32_t button_clicks = 0;

static void halt_with_error(const char *message) {
  Serial.println(message);
  while (true) {
    delay(1000);
  }
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t *>(pixel_map), width, height);
  lv_display_flush_ready(display);
}

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  data->state = LV_INDEV_STATE_RELEASED;
  if (!touch_ready) {
    return;
  }

  Gt911Point points{};
  if (!gt911_read(points) || points.count == 0) {
    return;
  }

  if (points.x[0] >= LCD43_WIDTH || points.y[0] >= LCD43_HEIGHT) {
    return;
  }
  data->point.x = points.x[0];
  data->point.y = points.y[0];
  data->state = LV_INDEV_STATE_PRESSED;
}

static void lvgl_tick(void *parameter) {
  (void)parameter;
  lv_tick_inc(kLvglTickPeriodMs);
}

static void button_clicked(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }
  lv_obj_t *counter = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
  lv_label_set_text_fmt(counter, "Touch count: %lu", (unsigned long)++button_clicks);
}

static void create_demo_ui() {
  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x10243A), 0);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "Waveshare LCD-4.3\nLVGL 9");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

  lv_obj_t *counter = lv_label_create(screen);
  lv_label_set_text(counter, "Touch count: 0");
  lv_obj_set_style_text_color(counter, lv_color_white(), 0);
  lv_obj_align(counter, LV_ALIGN_CENTER, 0, -45);

  lv_obj_t *button = lv_button_create(screen);
  lv_obj_set_size(button, 180, 70);
  lv_obj_align(button, LV_ALIGN_CENTER, 0, 45);
  lv_obj_add_event_cb(button, button_clicked, LV_EVENT_CLICKED, counter);

  lv_obj_t *button_label = lv_label_create(button);
  lv_label_set_text(button_label, "Touch me");
  lv_obj_center(button_label);

  lv_obj_t *slider = lv_slider_create(screen);
  lv_obj_set_width(slider, 300);
  lv_slider_set_value(slider, 65, LV_ANIM_OFF);
  lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -100);
}

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  delay(1000);

  Serial.println("Arduino GFX + LVGL v9 example");
  if (gfx == nullptr || !gfx->begin()) {
    halt_with_error("gfx->begin() failed!");
  }

  // Polling is intentional: the shared GT911 driver probes both valid
  // addresses and this board does not use GT911 INT or RST pins.
  touch_ready = gt911_begin();
  if (!touch_ready) {
    Serial.println("GT911 unavailable; continuing without touch");
  }

  lv_init();
  const size_t draw_buffer_bytes = LCD43_WIDTH * kDrawBufferHeight * sizeof(lv_color_t);
  draw_buffer_1 = static_cast<lv_color_t *>(heap_caps_malloc(draw_buffer_bytes, MALLOC_CAP_SPIRAM));
  draw_buffer_2 = static_cast<lv_color_t *>(heap_caps_malloc(draw_buffer_bytes, MALLOC_CAP_SPIRAM));
  if (draw_buffer_1 == nullptr || draw_buffer_2 == nullptr) {
    if (draw_buffer_1 != nullptr) {
      heap_caps_free(draw_buffer_1);
      draw_buffer_1 = nullptr;
    }
    if (draw_buffer_2 != nullptr) {
      heap_caps_free(draw_buffer_2);
      draw_buffer_2 = nullptr;
    }
    halt_with_error("LVGL draw buffer allocation failed!");
  }

  lv_display = lv_display_create(LCD43_WIDTH, LCD43_HEIGHT);
  if (lv_display == nullptr) {
    halt_with_error("LVGL display allocation failed!");
  }
  lv_display_set_flush_cb(lv_display, display_flush);
  lv_display_set_buffers(lv_display, draw_buffer_1, draw_buffer_2, draw_buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

  touchpad = lv_indev_create();
  if (touchpad == nullptr) {
    halt_with_error("LVGL input-device allocation failed!");
  }
  lv_indev_set_type(touchpad, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touchpad, touchpad_read);

  const esp_timer_create_args_t timer_args = {
    .callback = &lvgl_tick,
    .name = "lvgl_tick",
  };
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&timer_args, &timer) != ESP_OK ||
      esp_timer_start_periodic(timer, kLvglTickPeriodMs * 1000) != ESP_OK) {
    halt_with_error("LVGL tick timer setup failed!");
  }

  lv_display_set_dpi(lv_display, 150);
  create_demo_ui();
  Serial.println("Setup complete");
}

void loop() {
  lv_timer_handler();
  delay(kLvglTickPeriodMs);
}
