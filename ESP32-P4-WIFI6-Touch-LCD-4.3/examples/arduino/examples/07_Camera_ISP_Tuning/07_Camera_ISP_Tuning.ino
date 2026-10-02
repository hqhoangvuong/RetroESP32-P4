/*
 * OV5647 MIPI-CSI ISP / 3A tuning demo for the Waveshare
 * ESP32-P4-WIFI6-Touch-LCD-4.3 (480x800 DSI display).
 *
 * Serial commands (each followed by Enter):
 *   g <0..1023> gain, e <0..10000> exposure in us, a <0..255> AE target,
 *   v <0|1> vertical flip, h <0|1> horizontal flip, t <0|1> test pattern,
 *   s print current settings.
 */
#ifndef BOARD_HAS_PSRAM
#error "This program requires PSRAM enabled (enable PSRAM in the Tools menu)"
#endif

#include <Arduino_GFX_Library.h>
#include <ESP_Video.h>
#include "displays_config.h"

#define CAMERA_SCCB_PORT 0
#define CAMERA_SCCB_SCL  LCD43_SCL
#define CAMERA_SCCB_SDA  LCD43_SDA

ESPVideoClass video;
ESPVideoCaptureDevClass capture_dev;
const size_t kCaptureBufferCount = 2;
Arduino_GFX *gfx = lcd43_create();

struct IspSettings {
  int32_t gain = -1;
  int32_t exposure = -1;
  int32_t ae_target = -1;
  int32_t vflip = -1;
  int32_t hflip = -1;
  int32_t test_pattern = -1;
} settings;

static bool in_range(long value, long minimum, long maximum) {
  return value >= minimum && value <= maximum;
}

static bool apply_control(char op, long value) {
  bool accepted = false;
  switch (op) {
    case 'g':
      if (!in_range(value, 0, 1023)) return false;
      accepted = capture_dev.setSensorGain(value);
      if (accepted) settings.gain = value;
      break;
    case 'e':
      if (!in_range(value, 0, 10000)) return false;
      accepted = capture_dev.setSensorExposureTime(value);
      if (accepted) settings.exposure = value;
      break;
    case 'a':
      if (!in_range(value, 0, 255)) return false;
      accepted = capture_dev.setSensorAETargetLevel(value);
      if (accepted) settings.ae_target = value;
      break;
    case 'v':
      if (!in_range(value, 0, 1)) return false;
      accepted = capture_dev.setSensorVFlip(value != 0);
      if (accepted) settings.vflip = value;
      break;
    case 'h':
      if (!in_range(value, 0, 1)) return false;
      accepted = capture_dev.setSensorHFlip(value != 0);
      if (accepted) settings.hflip = value;
      break;
    case 't':
      if (!in_range(value, 0, 1)) return false;
      accepted = capture_dev.setSensorTestPattern(value != 0);
      if (accepted) settings.test_pattern = value;
      break;
    default:
      return false;
  }
  return accepted;
}

static bool parse_value(const String &command, long &value) {
  const char *cursor = command.c_str() + 1;
  while (*cursor == ' ' || *cursor == '\t') ++cursor;
  if (*cursor == '\0') return false;
  char *end = nullptr;
  value = strtol(cursor, &end, 10);
  if (end == cursor) return false;
  while (*end == ' ' || *end == '\t') ++end;
  return *end == '\0';
}

static void showSettings() {
  Serial.printf("gain=%ld exposure_us=%ld ae_target=%ld vflip=%ld hflip=%ld test_pattern=%ld\n",
                (long)settings.gain, (long)settings.exposure, (long)settings.ae_target,
                (long)settings.vflip, (long)settings.hflip, (long)settings.test_pattern);
}

static void handleSerial() {
  static String cmd;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      cmd.trim();
      if (cmd.length() > 0) {
        const char op = cmd[0];
        if (op == 's' && cmd.length() == 1) {
          showSettings();
        } else {
          long value = 0;
          if (!parse_value(cmd, value) || !apply_control(op, value)) {
            Serial.println("invalid value or sensor rejected control");
          }
          showSettings();
        }
      }
      cmd = "";
    } else {
      cmd += c;
    }
  }
}

bool initCamera() {
  ESPVideoCamConfigClass cam_config;
  if (!cam_config.begin(CAMERA_SCCB_PORT, CAMERA_SCCB_SCL, CAMERA_SCCB_SDA)) {
    Serial.println("SCCB config failed");
    return false;
  }
  ESPVideoCSIConfigClass csi_config;
  csi_config.begin(cam_config);
  if (!video.begin(csi_config) ||
      !capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, kCaptureBufferCount) ||
      !capture_dev.setFormat(ESP_VIDEO_FORMAT_RGB565) ||
      !capture_dev.startCapture()) {
    Serial.println("camera pipeline init failed");
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  delay(200);

  if (!gfx->begin()) {
    Serial.println("display begin failed!");
    return;
  }
  gfx->fillScreen(RGB565_BLACK);
  if (!initCamera()) {
    gfx->setTextColor(RGB565_RED);
    gfx->println("Camera init failed - connect an OV5647 module");
    return;
  }
  Serial.println("ISP tuning ready. Commands: g/e/a/v/h/t/s");
  showSettings();
}

void loop() {
  handleSerial();
  if (!capture_dev.isOpened() || !capture_dev.isCaptureStarted()) {
    delay(500);
    return;
  }

  ESPVideoBufferClass buffer = capture_dev.captureBuffer();
  if (!buffer.valid()) {
    delay(5);
    return;
  }
  const uint32_t w = buffer.getWidth();
  const uint32_t h = buffer.getHeight();
  if (w > 0 && h > 0 && buffer.formatType() == ESP_VIDEO_FORMAT_RGB565) {
    const size_t frame_bytes = buffer.size();
    if (frame_bytes % h != 0 || (frame_bytes / h) % sizeof(uint16_t) != 0) {
      Serial.println("unexpected RGB565 frame size");
      return;
    }
    const size_t stride_pixels = (frame_bytes / h) / sizeof(uint16_t);
    if (stride_pixels < w) {
      Serial.println("RGB565 frame stride is shorter than its width");
      return;
    }
    const int16_t panel_w = gfx->width();
    const int16_t panel_h = gfx->height();
    const uint32_t draw_w = w < (uint32_t)panel_w ? w : (uint32_t)panel_w;
    const uint32_t draw_h = h < (uint32_t)panel_h ? h : (uint32_t)panel_h;
    const uint32_t src_x = (w - draw_w) / 2;
    const uint32_t src_y = (h - draw_h) / 2;
    const int16_t dst_x = (panel_w - (int16_t)draw_w) / 2;
    const int16_t dst_y = (panel_h - (int16_t)draw_h) / 2;
    const uint16_t *pixels = (const uint16_t *)buffer.data();
    for (uint32_t y = 0; y < draw_h; ++y) {
      gfx->draw16bitRGBBitmap(dst_x, dst_y + (int16_t)y,
                              (uint16_t *)(pixels + (size_t)(src_y + y) * stride_pixels + src_x),
                              (int16_t)draw_w, 1);
    }

    gfx->setTextColor(RGB565_BLACK, RGB565_WHITE);
    gfx->setCursor(4, 4);
    gfx->printf("gain=%ld exp=%ldus ae=%ld", (long)settings.gain,
                (long)settings.exposure, (long)settings.ae_target);
  }
  // The capture driver reclaims buffer when this stack object goes out of scope.
}
