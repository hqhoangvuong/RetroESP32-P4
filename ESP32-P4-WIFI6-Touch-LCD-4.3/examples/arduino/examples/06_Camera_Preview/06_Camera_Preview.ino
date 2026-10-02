/*
 * OV5647 MIPI-CSI camera preview for the Waveshare
 * ESP32-P4-WIFI6-Touch-LCD-4.3 (480x800 DSI display).
 *
 * Uses ESP_Video from arduino-esp32 3.3.11. The OV5647 SCCB bus is on
 * GPIO7/GPIO8 and the MIPI-CSI lanes are wired on the board. The ISP emits
 * RGB565 frames for direct display. Frames are centred and cropped to the
 * 480x800 panel without treating a cropped frame as a contiguous bitmap.
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

bool initCamera() {
  ESPVideoCamConfigClass cam_config;
  if (!cam_config.begin(CAMERA_SCCB_PORT, CAMERA_SCCB_SCL, CAMERA_SCCB_SDA)) {
    Serial.println("SCCB config failed");
    return false;
  }

  ESPVideoCSIConfigClass csi_config;
  csi_config.begin(cam_config);
  if (!video.begin(csi_config)) {
    Serial.println("CSI camera init failed");
    return false;
  }
  if (!capture_dev.begin(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, kCaptureBufferCount)) {
    Serial.println("capture device open failed");
    return false;
  }
  if (!capture_dev.setFormat(ESP_VIDEO_FORMAT_RGB565)) {
    Serial.println("RGB565 format request failed");
    return false;
  }
  if (!capture_dev.startCapture()) {
    Serial.println("start capture failed");
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
  gfx->setTextColor(RGB565_WHITE);
  gfx->println("Camera preview starting...");

  if (!initCamera()) {
    gfx->setTextColor(RGB565_RED);
    gfx->println("Camera init failed - connect an OV5647 module");
  }
}

void loop() {
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

    // Draw each cropped row using the pitch derived from the dequeued buffer.
    for (uint32_t y = 0; y < draw_h; ++y) {
      gfx->draw16bitRGBBitmap(dst_x, dst_y + (int16_t)y,
                              (uint16_t *)(pixels + (size_t)(src_y + y) * stride_pixels + src_x),
                              (int16_t)draw_w, 1);
    }
  }
  // The capture driver reclaims buffer when this stack object goes out of scope.
}
