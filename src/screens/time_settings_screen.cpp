#include "time_settings_screen.h"
#include "../hal/display.h"
#include "../services/time_svc.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

TimeSettingsScreen time_settings_screen;

namespace {
constexpr const char* TAG = "time";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;

// Round-display safe-area guidance: keep widths ≤ 340 at top/bottom.
constexpr int CW         = 466;
constexpr int CENTER_X   = CW / 2;
constexpr int LIST_W     = 280;
constexpr int LIST_X     = (CW - LIST_W) / 2;
constexpr int LIST_TOP   = 110;
// 8 presets × 20 px = 160 px, ending at y=270; back button starts at y=280,
// which leaves a clean 10 px gap.
constexpr int LIST_ROW_H = 20;
constexpr int BUTTON_H   = 32;
constexpr int BACK_W     = 96;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  const int ch_w = 6 * sz;
  g->setCursor(CENTER_X - (int)strlen(s) * ch_w / 2, y);
  g->print(s);
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

void TimeSettingsScreen::on_appear() {
  dirty_ = true;
}

void TimeSettingsScreen::update(uint32_t now_ms) {
  // Refresh every 500 ms so the live clock ticks.
  if (!dirty_ && (now_ms - last_render_ms_) < 500) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  using namespace ::robimon::services;

  draw_centered_text(g, 8, 3, COLOR_FG, "time");

  // Live clock
  struct tm lt;
  if (time_svc::get_local_time(&lt)) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    draw_centered_text(g, 44, 3, time_svc::is_synced() ? COLOR_OK : COLOR_DIM, buf);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             time_svc::current_tz_name());
    draw_centered_text(g, 80, 2, COLOR_DIM, buf);
  } else {
    draw_centered_text(g, 50, 2, COLOR_DIM, "not synced");
    char buf[40];
    snprintf(buf, sizeof(buf), "tz: %s", time_svc::current_tz_name());
    draw_centered_text(g, 80, 2, COLOR_DIM, buf);
  }

  // Timezone preset list
  const int n = time_svc::N_TZ_PRESETS;
  for (int i = 0; i < n; ++i) {
    const int y = LIST_TOP + i * LIST_ROW_H;
    const bool is_current = (strcmp(time_svc::TZ_PRESETS[i].posix_tz,
                                     time_svc::current_tz_string()) == 0);
    const uint16_t color = is_current ? COLOR_OK : COLOR_FG;
    g->drawRoundRect(LIST_X, y, LIST_W, LIST_ROW_H - 2, 4, color);
    g->setTextColor(color);
    g->setTextSize(2);
    g->setCursor(LIST_X + 12, y + LIST_ROW_H / 2 - 8 - 1);
    g->print(time_svc::TZ_PRESETS[i].name);
    if (is_current) {
      g->setCursor(LIST_X + LIST_W - 28, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(">");
    }
  }

  draw_centered_button(g, 320 - BUTTON_H - 8, BACK_W, BUTTON_H, COLOR_DIM, "back");
  ::robimon::hal::display::flush();
}

void TimeSettingsScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  using namespace ::robimon::services;

  // Back
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  // Preset rows
  const int n = time_svc::N_TZ_PRESETS;
  for (int i = 0; i < n; ++i) {
    const int y = LIST_TOP + i * LIST_ROW_H;
    if (panel_x >= LIST_X && panel_x < LIST_X + LIST_W &&
        canvas_y >= y && canvas_y < y + LIST_ROW_H - 2) {
      LOG_I(TAG, "tz preset -> %s", time_svc::TZ_PRESETS[i].name);
      time_svc::set_tz_preset(i);
      dirty_ = true;
      return;
    }
  }
}

}  // namespace robimon::screens
