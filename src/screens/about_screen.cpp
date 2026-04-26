#include "about_screen.h"
#include "../hal/display.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <string.h>

namespace robimon::screens {

AboutScreen about_screen;

namespace {
constexpr const char* TAG = "about";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;
constexpr int OTA_W    = 110;
constexpr int OTA_Y    = 250;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}
void draw_label_value(Arduino_GFX* g, int y, const char* label, const char* value) {
  // Two columns centered around CENTER_X with a small gap.
  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  const int lw = (int)strlen(label) * 12;
  const int vw = (int)strlen(value) * 12;
  const int total = lw + 12 + vw;
  const int x_label = CENTER_X - total / 2;
  g->setCursor(x_label, y);
  g->print(label);
  g->setTextColor(COLOR_FG);
  g->setCursor(x_label + lw + 12, y);
  g->print(value);
}
void draw_centered_button(Arduino_GFX* g, int y, int w, int h, uint16_t color, const char* label) {
  const int x = CENTER_X - w / 2;
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  g->setCursor(CENTER_X - (int)strlen(label) * 12 / 2, y + h / 2 - 8);
  g->print(label);
}

}  // namespace

void AboutScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 1000) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 3, COLOR_FG, "about");

  char buf[40];

  // Version
  draw_label_value(g, 50, "version", ROBIMON_VERSION);
  // Board
  draw_label_value(g, 76, "board", "ESP32-S3-AMOLED-1.75");

  // Memory
  const size_t heap_kb  = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL) / 1024;
  const size_t psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
  snprintf(buf, sizeof(buf), "%uK / %uK", (unsigned)heap_kb, (unsigned)psram_kb);
  draw_label_value(g, 102, "free", buf);

  // MAC
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  draw_label_value(g, 128, "MAC", buf);

  // Uptime
  const uint64_t up_us = (uint64_t)esp_timer_get_time();
  const uint32_t up_s  = (uint32_t)(up_us / 1000000ULL);
  const uint32_t h = up_s / 3600;
  const uint32_t m = (up_s % 3600) / 60;
  const uint32_t s = up_s % 60;
  snprintf(buf, sizeof(buf), "%luh %02lum %02lus",
           (unsigned long)h, (unsigned long)m, (unsigned long)s);
  draw_label_value(g, 154, "uptime", buf);

  // App partition usage (best-effort)
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  if (p) {
    snprintf(buf, sizeof(buf), "%uK / %uK",
             (unsigned)(p->size / 1024 - 1024 / 4),  // rough placeholder, hard to query
             (unsigned)(p->size / 1024));
    draw_label_value(g, 180, "app", buf);
  }

  // OTA stub button
  draw_centered_button(g, OTA_Y, OTA_W, BUTTON_H, COLOR_DIM, "OTA (soon)");

  draw_centered_button(g, 320 - BUTTON_H - 8, BACK_W, BUTTON_H, COLOR_FG, "back");
  ::robimon::hal::display::flush();
}

void AboutScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  // Back
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  // OTA stub — logs only for now.
  if (panel_x >= CENTER_X - OTA_W / 2 && panel_x < CENTER_X + OTA_W / 2 &&
      canvas_y >= OTA_Y && canvas_y < OTA_Y + BUTTON_H) {
    LOG_I(TAG, "OTA tapped — not implemented yet");
    return;
  }
}

}  // namespace robimon::screens
