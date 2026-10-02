#ifndef BOARD_HAS_PSRAM
#error "This example requires PSRAM. Enable PSRAM in the Arduino Tools menu."
#endif

#include <Arduino_GFX_Library.h>
#include <WiFi.h>

#include "displays_config.h"

static constexpr int32_t kRssiCeiling = -40;
static constexpr int32_t kRssiFloor = -100;
static constexpr uint8_t kFirstChannel = 1;
static constexpr uint8_t kLastChannel = 14;

static const uint16_t channel_colors[] = {
  RGB565_RED, RGB565_ORANGE, RGB565_YELLOW, RGB565_GREEN, RGB565_CYAN, RGB565_BLUE, RGB565_MAGENTA,
  RGB565_RED, RGB565_ORANGE, RGB565_YELLOW, RGB565_GREEN, RGB565_CYAN, RGB565_BLUE, RGB565_MAGENTA,
};

static Arduino_GFX *gfx = lcd43_create();
static int16_t display_width;
static int16_t display_height;
static int16_t text_size;
static int16_t banner_height;
static int16_t graph_baseline;
static int16_t graph_height;
static int16_t channel_width;
static int16_t signal_width;
static bool display_ready = false;

static bool valid_channel(int32_t channel) {
  return channel >= kFirstChannel && channel <= kLastChannel;
}

static bool match_bssid_prefix(const uint8_t *first, const uint8_t *second) {
  for (uint8_t i = 0; i < 5; ++i) {
    if (first[i] != second[i]) {
      return false;
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  lcd43_backlight(true);
  Serial.println("Arduino_GFX Wi-Fi Analyzer example");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (gfx == nullptr || !gfx->begin()) {
    Serial.println("gfx->begin() failed!");
    return;
  }
  display_ready = true;

  display_width = gfx->width();
  display_height = gfx->height();
  text_size = display_height < 200 ? 1 : 2;
  banner_height = text_size * 3 * 4;
  graph_baseline = display_height - 20;
  graph_height = graph_baseline - banner_height - 30;
  channel_width = display_width / 17;
  signal_width = channel_width * 2;

  gfx->setTextSize(text_size);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_RED);
  gfx->setCursor(0, 0);
  gfx->print("ESP");
  gfx->setTextColor(RGB565_WHITE);
  gfx->print(" WiFi Analyzer");
}

void loop() {
  if (!display_ready) {
    delay(1000);
    return;
  }

  uint8_t ap_count[kLastChannel] = {};
  int32_t noise[kLastChannel];
  int32_t peak[kLastChannel];
  int16_t peak_id[kLastChannel];
  for (uint8_t i = 0; i < kLastChannel; ++i) {
    noise[i] = kRssiFloor;
    peak[i] = kRssiFloor;
    peak_id[i] = -1;
  }

  const int network_count = WiFi.scanNetworks(false, true, true, 500);
  gfx->fillRect(0, banner_height, display_width, display_height - banner_height, RGB565_BLACK);
  gfx->setTextSize(1);

  if (network_count <= 0) {
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(0, banner_height);
    gfx->println(network_count == 0 ? "no networks found" : "Wi-Fi scan failed");
    WiFi.scanDelete();
    delay(1000);
    return;
  }

  for (int i = 0; i < network_count; ++i) {
    const int32_t channel = WiFi.channel(i);
    if (!valid_channel(channel)) {
      continue;
    }
    const uint8_t index = channel - kFirstChannel;
    const int32_t rssi = WiFi.RSSI(i);
    const uint8_t *bssid = WiFi.BSSID(i);

    if (peak[index] < rssi) {
      peak[index] = rssi;
      peak_id[index] = i;
    }

    bool duplicate_ssid = false;
    for (int j = 0; j < i; ++j) {
      if (WiFi.channel(j) == channel && match_bssid_prefix(WiFi.BSSID(j), bssid)) {
        duplicate_ssid = true;
        break;
      }
    }
    if (duplicate_ssid) {
      continue;
    }

    ++ap_count[index];
    int32_t contribution = rssi - kRssiFloor;
    contribution *= contribution;
    for (int32_t neighbor = channel - 4; neighbor <= channel + 4; ++neighbor) {
      if (valid_channel(neighbor)) {
        noise[neighbor - kFirstChannel] += contribution;
      }
    }
  }

  for (int i = 0; i < network_count; ++i) {
    const int32_t channel = WiFi.channel(i);
    if (!valid_channel(channel)) {
      continue;
    }
    const uint8_t index = channel - kFirstChannel;
    int32_t rssi = WiFi.RSSI(i);
    const uint16_t color = channel_colors[index];
    const int16_t height = constrain(map(rssi, kRssiFloor, kRssiCeiling, 1, graph_height), 1, graph_height);
    int16_t offset = (channel + 1) * channel_width;

    if (rssi < kRssiFloor) {
      rssi = kRssiFloor;
    }

    gfx->startWrite();
    gfx->writeEllipseHelper(offset, graph_baseline + 1, signal_width, height, 0b0011, color);
    gfx->endWrite();

    if (i == peak_id[index]) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        ssid = WiFi.BSSIDstr(i);
      }
      const int16_t label_width = (ssid.length() + 6) * 6;
      if (label_width > display_width) {
        offset = 0;
      } else {
        offset -= signal_width;
        if (offset + label_width > display_width) {
          offset = display_width - label_width;
        }
      }
      gfx->setTextColor(color);
      gfx->setCursor(offset, graph_baseline - 10 - height);
      gfx->print(ssid);
      gfx->print('(');
      gfx->print(rssi);
      gfx->print(')');
      if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
        gfx->print('*');
      }
    }
  }

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(0, banner_height);
  gfx->print(network_count);
  gfx->print(" networks found, lesser noise channels: ");

  int32_t minimum_noise = noise[0];
  for (uint8_t channel = 2; channel <= 11; ++channel) {
    const uint8_t index = channel - kFirstChannel;
    if (noise[index] < minimum_noise) {
      minimum_noise = noise[index];
    }
  }
  bool first_channel = true;
  for (uint8_t channel = kFirstChannel; channel <= 11; ++channel) {
    if (noise[channel - kFirstChannel] == minimum_noise) {
      if (!first_channel) {
        gfx->print(", ");
      }
      gfx->print(channel);
      first_channel = false;
    }
  }

  gfx->drawFastHLine(0, graph_baseline, display_width, RGB565_WHITE);
  for (uint8_t channel = kFirstChannel; channel <= kLastChannel; ++channel) {
    const uint8_t index = channel - kFirstChannel;
    const int16_t offset = (channel + 1) * channel_width;
    gfx->setTextColor(channel_colors[index]);
    gfx->setCursor(offset - (channel < 10 ? 3 : 6), graph_baseline + 2);
    gfx->print(channel);
    if (ap_count[index] > 0) {
      gfx->setCursor(offset - (ap_count[index] < 10 ? 9 : 12), graph_baseline + 10);
      gfx->print('{');
      gfx->print(ap_count[index]);
      gfx->print('}');
    }
  }

  WiFi.scanDelete();
  delay(1000);
}
