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
  g->setCursor(cx - hdr_w / 2, 24);
  g->print(hdr);

  // Bell illustration. A rounded bell body (top arc + rectangular base)
  // and a small clapper below; outlined so it reads at distance and
  // doesn't fight the cyan-on-black aesthetic.
  const int bell_cy = 110;
  const int bell_w  = 56;
  const int bell_h  = 60;
  // Top hemisphere (semi-circle)
  g->fillCircle(cx, bell_cy, bell_w / 2, COLOR_FG);
  g->fillRect(cx - bell_w / 2, bell_cy, bell_w, 1, COLOR_FG);
  // Body — rounded rectangle below the dome
  g->fillRoundRect(cx - bell_w / 2 - 4, bell_cy, bell_w + 8, bell_h - 28, 4, COLOR_FG);
  // Cut out the inside so the bell is an outline rather than a slab
  g->fillCircle(cx, bell_cy, bell_w / 2 - 4, COLOR_BG);
  g->fillRoundRect(cx - bell_w / 2, bell_cy, bell_w, bell_h - 32, 2, COLOR_BG);
  // Lip + clapper
  g->fillRoundRect(cx - bell_w / 2 - 6, bell_cy + bell_h - 32,
                   bell_w + 12, 6, 2, COLOR_FG);
  g->fillCircle(cx, bell_cy + bell_h - 18, 4, COLOR_FG);

  // Empty-state message — bright, large, friendly. Spells out the path
  // (long-press → settings → alarms → add) so a kid who lands here cold
  // knows what to do next.
  g->setTextSize(3);
  g->setTextColor(COLOR_FG);
  const char* line1 = "no alarms yet!";
  const int   l1_w  = (int)strlen(line1) * 18;
  g->setCursor(cx - l1_w / 2, 188);
  g->print(line1);

  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  const char* line2 = "hold the screen";
  const char* line3 = "to open settings";
  const int   l2_w  = (int)strlen(line2) * 12;
  const int   l3_w  = (int)strlen(line3) * 12;
  g->setCursor(cx - l2_w / 2, 232);
  g->print(line2);
  g->setCursor(cx - l3_w / 2, 256);
  g->print(line3);

  ::robimon::hal::display::flush();
}

void AlarmsScreen::on_tap(int /*x*/, int /*y*/) {
  // No-op for now. When alarms exist, tap on a row will dismiss/snooze
  // (per the kid-facing spec: dismiss by tap, snooze by swipe). Swipes are
  // already routed by the screen manager so we don't see them here.
}

}  // namespace robimon::screens
