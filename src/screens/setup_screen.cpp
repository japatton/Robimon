#include "setup_screen.h"
#include "../hal/display.h"
#include "../services/setup_portal.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

SetupScreen setup_screen;

namespace {
constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}

}  // namespace

void SetupScreen::update(uint32_t now_ms) {
  // Render every second so the heartbeat at the bottom ticks visibly.
  if (!dirty_ && (now_ms - last_render_ms_) < 1000) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  // Header
  draw_centered_text(g, 14, 3, COLOR_FG, "setup");
  draw_centered_text(g, 50, 2, COLOR_DIM, "first-time configuration");

  // Step 1
  draw_centered_text(g, 84,  2, COLOR_DIM, "1. join wifi:");
  draw_centered_text(g, 106, 3, COLOR_OK,
                     ::robimon::services::setup_portal::AP_SSID);
  draw_centered_text(g, 138, 1, COLOR_DIM, "password:");
  draw_centered_text(g, 152, 2, COLOR_OK,
                     ::robimon::services::setup_portal::AP_PASSWORD);

  // Step 2
  draw_centered_text(g, 184, 2, COLOR_DIM, "2. open in browser:");
  char url_buf[40];
  snprintf(url_buf, sizeof(url_buf), "http://%s",
           ::robimon::services::setup_portal::AP_IP);
  draw_centered_text(g, 208, 2, COLOR_OK, url_buf);

  // Step 3
  draw_centered_text(g, 240, 2, COLOR_DIM, "3. fill in the form");

  // Heartbeat at bottom — confirms the device isn't frozen
  const uint32_t s = now_ms / 1000;
  const char* dots = (s % 4 == 0) ? "."   : (s % 4 == 1) ? ".."
                   : (s % 4 == 2) ? "..." : "....";
  g->setTextColor(COLOR_DIM);
  g->setTextSize(2);
  g->setCursor(CENTER_X - 60, 280);
  g->print("waiting ");
  g->print(dots);

  ::robimon::hal::display::flush();
}

}  // namespace robimon::screens
