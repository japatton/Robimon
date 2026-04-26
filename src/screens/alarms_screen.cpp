#include "alarms_screen.h"
#include "../hal/display.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace robimon::screens {

AlarmsScreen alarms_screen;

namespace {
constexpr uint16_t COLOR_BG       = 0x0000;
constexpr uint16_t COLOR_FG       = 0x055F;   // matches face cyan
constexpr uint16_t COLOR_DIM      = 0x01F6;
constexpr uint32_t FRAME_MS       = 100;      // 10 FPS — enough for a live counter
}  // namespace

void AlarmsScreen::on_appear() {
  dirty_ = true;
  appeared_ms_ = millis();
}

void AlarmsScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < FRAME_MS) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;

  g->fillScreen(COLOR_BG);

  const int cx = g->width() / 2;

  // Header
  g->setTextColor(COLOR_FG);
  g->setTextSize(4);
  const char* hdr = "alarms";
  const int hdr_w = (int)strlen(hdr) * 24;
  g->setCursor(cx - hdr_w / 2, 30);
  g->print(hdr);

  // Empty-state message
  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  const char* empty1 = "no alarms set";
  const char* empty2 = "add from settings";
  const int e1_w = (int)strlen(empty1) * 12;
  const int e2_w = (int)strlen(empty2) * 12;
  g->setCursor(cx - e1_w / 2, g->height() / 2 - 16);
  g->print(empty1);
  g->setCursor(cx - e2_w / 2, g->height() / 2 + 8);
  g->print(empty2);

  // Heartbeat: time on this screen, ticking up each second. Confirms the
  // screen is alive (not frozen) and the swipe-back path actually leaves it.
  const uint32_t secs_here = (now_ms - appeared_ms_) / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%lus on this screen", (unsigned long)secs_here);
  const int b_w = (int)strlen(buf) * 12;
  g->setCursor(cx - b_w / 2, g->height() - 50);
  g->print(buf);

  ::robimon::hal::display::flush();
}

void AlarmsScreen::on_tap(int /*x*/, int /*y*/) {
  // No-op for now. When alarms exist, tap on a row will dismiss/snooze
  // (per the kid-facing spec: dismiss by tap, snooze by swipe). Swipes are
  // already routed by the screen manager so we don't see them here.
}

}  // namespace robimon::screens
