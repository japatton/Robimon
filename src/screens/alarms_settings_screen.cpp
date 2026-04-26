#include "alarms_settings_screen.h"
#include "alarm_edit_screen.h"
#include "../hal/display.h"
#include "../services/alarm_mgr.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

AlarmsSettingsScreen alarms_settings_screen;

namespace {
constexpr const char* TAG = "alm-set";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OFF = 0x4208;   // dim grey for disabled alarms

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int LIST_W   = 380;
constexpr int LIST_X   = (CW - LIST_W) / 2;
constexpr int LIST_TOP = 50;
constexpr int LIST_ROW_H = 36;
constexpr int LIST_VISIBLE_ROWS = 5;

constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;
constexpr int ADD_W    = 96;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}
void draw_button(Arduino_GFX* g, int x, int y, int w, int h,
                  uint16_t color, const char* label) {
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  g->setCursor(x + w / 2 - (int)strlen(label) * 12 / 2, y + h / 2 - 8);
  g->print(label);
}

}  // namespace

void AlarmsSettingsScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 3, COLOR_FG, "alarms");

  using namespace ::robimon::services;
  const int n = alarm_mgr::count();
  if (n == 0) {
    draw_centered_text(g, 100, 2, COLOR_DIM, "no alarms yet");
    draw_centered_text(g, 130, 2, COLOR_DIM, "tap + to add");
  } else {
    g->setTextSize(2);
    const int show = (n < LIST_VISIBLE_ROWS) ? n : LIST_VISIBLE_ROWS;
    for (int i = 0; i < show; ++i) {
      const auto* a = alarm_mgr::get(i);
      if (!a) continue;
      const int y = LIST_TOP + i * LIST_ROW_H;
      const uint16_t color = a->enabled ? COLOR_FG : COLOR_OFF;
      g->drawRoundRect(LIST_X, y, LIST_W, LIST_ROW_H - 4, 4, color);

      // HH:MM
      char tbuf[8];
      snprintf(tbuf, sizeof(tbuf), "%02d:%02d", (int)a->hour, (int)a->minute);
      g->setTextColor(color);
      g->setCursor(LIST_X + 12, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(tbuf);

      // Days
      char dbuf[20];
      alarm_mgr::days_summary(a->days_mask, dbuf, sizeof(dbuf));
      g->setCursor(LIST_X + 90, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(dbuf);

      // Enabled marker on right
      g->setCursor(LIST_X + LIST_W - 28, y + LIST_ROW_H / 2 - 8 - 1);
      g->print(a->enabled ? "on" : "off");
    }
  }

  // Add + back at bottom
  const int bottom_y = 320 - BUTTON_H - 8;
  const int total = ADD_W + 12 + BACK_W;
  const int add_x = CENTER_X - total / 2;
  const int back_x = add_x + ADD_W + 12;
  draw_button(g, add_x,  bottom_y, ADD_W,  BUTTON_H, COLOR_FG,  "+ add");
  draw_button(g, back_x, bottom_y, BACK_W, BUTTON_H, COLOR_DIM, "back");

  ::robimon::hal::display::flush();
}

void AlarmsSettingsScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  using namespace ::robimon::services;

  // List rows: edit existing alarm
  const int n = alarm_mgr::count();
  const int show = (n < LIST_VISIBLE_ROWS) ? n : LIST_VISIBLE_ROWS;
  for (int i = 0; i < show; ++i) {
    const int y = LIST_TOP + i * LIST_ROW_H;
    if (panel_x >= LIST_X && panel_x < LIST_X + LIST_W &&
        canvas_y >= y && canvas_y < y + LIST_ROW_H - 4) {
      LOG_I(TAG, "edit alarm %d", i);
      const auto* a = alarm_mgr::get(i);
      if (a) {
        alarm_edit_screen.edit_idx_ = i;
        alarm_edit_screen.staging_ = *a;
        ::robimon::ui::screen_mgr::push_modal(&alarm_edit_screen);
      }
      return;
    }
  }

  // Bottom buttons
  const int bottom_y = 320 - BUTTON_H - 8;
  const int total = ADD_W + 12 + BACK_W;
  const int add_x = CENTER_X - total / 2;
  const int back_x = add_x + ADD_W + 12;

  if (panel_x >= add_x && panel_x < add_x + ADD_W &&
      canvas_y >= bottom_y && canvas_y < bottom_y + BUTTON_H) {
    if (alarm_mgr::count() >= alarm_mgr::MAX_ALARMS) {
      LOG_W(TAG, "max alarms reached (%d)", alarm_mgr::MAX_ALARMS);
      return;
    }
    LOG_I(TAG, "add new alarm");
    // Default new alarm: 7:00 every day, neutral expression, enabled.
    alarm_edit_screen.edit_idx_ = -1;
    alarm_edit_screen.staging_ = { /*enabled=*/true, /*hour=*/7, /*minute=*/0,
                                    /*days_mask=*/0, /*expression=*/0, "" };
    snprintf(alarm_edit_screen.staging_.label,
             sizeof(alarm_edit_screen.staging_.label), "alarm");
    ::robimon::ui::screen_mgr::push_modal(&alarm_edit_screen);
    return;
  }
  if (panel_x >= back_x && panel_x < back_x + BACK_W &&
      canvas_y >= bottom_y && canvas_y < bottom_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }
}

}  // namespace robimon::screens
