#include "display_settings_screen.h"
#include "../hal/display.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

DisplaySettingsScreen display_settings_screen;

namespace {
constexpr const char* TAG = "disp";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;

// Brightness presets (1..8). The CO5300 setBrightness takes 0..255.
constexpr uint8_t BRIGHT_LEVELS[] = { 16, 48, 80, 112, 144, 176, 208, 240 };
constexpr int     N_BRIGHT = sizeof(BRIGHT_LEVELS) / sizeof(BRIGHT_LEVELS[0]);
constexpr int     BRIGHT_BTN_W = 36;
constexpr int     BRIGHT_BTN_H = 32;
constexpr int     BRIGHT_GAP   = 4;
constexpr int     BRIGHT_ROW_Y = 60;

constexpr const char* KEY_BRIGHT_IDX = "disp_b";

// Idle dim and sleep options. Stored as seconds.
struct IdleOpt { uint16_t secs; const char* label; };
constexpr IdleOpt DIM_OPTS[]   = { {30,"30s"}, {60,"1m"}, {120,"2m"}, {300,"5m"}, {0,"never"} };
constexpr IdleOpt SLEEP_OPTS[] = { {60,"1m"}, {300,"5m"}, {600,"10m"}, {1800,"30m"}, {0,"never"} };
constexpr int N_DIM   = sizeof(DIM_OPTS)   / sizeof(DIM_OPTS[0]);
constexpr int N_SLEEP = sizeof(SLEEP_OPTS) / sizeof(SLEEP_OPTS[0]);
constexpr int OPT_BTN_W = 60;
constexpr int OPT_BTN_H = 28;
constexpr int OPT_GAP   = 4;
constexpr int DIM_ROW_Y   = 130;
constexpr int SLEEP_ROW_Y = 195;

constexpr const char* KEY_DIM_S   = "disp_dim_s";
constexpr const char* KEY_SLEEP_S = "disp_slp_s";

uint8_t  current_bright_idx() {
  return (uint8_t)::robimon::services::config::get_uint(KEY_BRIGHT_IDX, 5);
}
uint16_t current_dim_secs() {
  return (uint16_t)::robimon::services::config::get_uint(KEY_DIM_S, 60);
}
uint16_t current_sleep_secs() {
  return (uint16_t)::robimon::services::config::get_uint(KEY_SLEEP_S, 300);
}

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
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

void draw_label(Arduino_GFX* g, int y, const char* s) {
  g->setTextColor(COLOR_DIM);
  g->setTextSize(2);
  g->setCursor(CENTER_X - (int)strlen(s) * 12 / 2, y);
  g->print(s);
}

}  // namespace

void DisplaySettingsScreen::on_appear() {
  dirty_ = true;
}

void DisplaySettingsScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 3, COLOR_FG, "display");

  // Brightness row (8 segments)
  draw_label(g, 38, "brightness");
  const uint8_t bidx = current_bright_idx();
  const int total_w = N_BRIGHT * BRIGHT_BTN_W + (N_BRIGHT - 1) * BRIGHT_GAP;
  const int x0 = CENTER_X - total_w / 2;
  for (int i = 0; i < N_BRIGHT; ++i) {
    const int x = x0 + i * (BRIGHT_BTN_W + BRIGHT_GAP);
    const bool selected = (i == bidx);
    const uint16_t color = selected ? COLOR_FG : COLOR_DIM;
    g->drawRoundRect(x, BRIGHT_ROW_Y, BRIGHT_BTN_W, BRIGHT_BTN_H, 4, color);
    if (selected) {
      g->fillRoundRect(x + 4, BRIGHT_ROW_Y + 4, BRIGHT_BTN_W - 8, BRIGHT_BTN_H - 8, 3, color);
    }
  }

  // Dim row
  draw_label(g, 110, "idle dim");
  const uint16_t dim_s = current_dim_secs();
  const int dim_total_w = N_DIM * OPT_BTN_W + (N_DIM - 1) * OPT_GAP;
  const int dim_x0 = CENTER_X - dim_total_w / 2;
  for (int i = 0; i < N_DIM; ++i) {
    const int x = dim_x0 + i * (OPT_BTN_W + OPT_GAP);
    const bool selected = (DIM_OPTS[i].secs == dim_s);
    const uint16_t color = selected ? COLOR_FG : COLOR_DIM;
    g->drawRoundRect(x, DIM_ROW_Y, OPT_BTN_W, OPT_BTN_H, 4, color);
    g->setTextColor(color);
    g->setTextSize(2);
    const int lw = (int)strlen(DIM_OPTS[i].label) * 12;
    g->setCursor(x + OPT_BTN_W / 2 - lw / 2, DIM_ROW_Y + OPT_BTN_H / 2 - 8);
    g->print(DIM_OPTS[i].label);
  }

  // Sleep row
  draw_label(g, 175, "sleep");
  const uint16_t sleep_s = current_sleep_secs();
  const int sleep_total_w = N_SLEEP * OPT_BTN_W + (N_SLEEP - 1) * OPT_GAP;
  const int sleep_x0 = CENTER_X - sleep_total_w / 2;
  for (int i = 0; i < N_SLEEP; ++i) {
    const int x = sleep_x0 + i * (OPT_BTN_W + OPT_GAP);
    const bool selected = (SLEEP_OPTS[i].secs == sleep_s);
    const uint16_t color = selected ? COLOR_FG : COLOR_DIM;
    g->drawRoundRect(x, SLEEP_ROW_Y, OPT_BTN_W, OPT_BTN_H, 4, color);
    g->setTextColor(color);
    g->setTextSize(2);
    const int lw = (int)strlen(SLEEP_OPTS[i].label) * 12;
    g->setCursor(x + OPT_BTN_W / 2 - lw / 2, SLEEP_ROW_Y + OPT_BTN_H / 2 - 8);
    g->print(SLEEP_OPTS[i].label);
  }

  draw_centered_button(g, 320 - BUTTON_H - 8, BACK_W, BUTTON_H, COLOR_DIM, "back");
  ::robimon::hal::display::flush();
}

void DisplaySettingsScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  // Back
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  // Brightness segments
  const int total_w = N_BRIGHT * BRIGHT_BTN_W + (N_BRIGHT - 1) * BRIGHT_GAP;
  const int x0 = CENTER_X - total_w / 2;
  if (canvas_y >= BRIGHT_ROW_Y && canvas_y < BRIGHT_ROW_Y + BRIGHT_BTN_H) {
    for (int i = 0; i < N_BRIGHT; ++i) {
      const int x = x0 + i * (BRIGHT_BTN_W + BRIGHT_GAP);
      if (panel_x >= x && panel_x < x + BRIGHT_BTN_W) {
        ::robimon::services::config::set_uint(KEY_BRIGHT_IDX, (uint32_t)i);
        ::robimon::hal::display::set_brightness(BRIGHT_LEVELS[i]);
        LOG_I(TAG, "brightness -> level %d (%u/255)", i, BRIGHT_LEVELS[i]);
        dirty_ = true;
        return;
      }
    }
  }

  // Dim row
  if (canvas_y >= DIM_ROW_Y && canvas_y < DIM_ROW_Y + OPT_BTN_H) {
    const int dim_total_w = N_DIM * OPT_BTN_W + (N_DIM - 1) * OPT_GAP;
    const int dim_x0 = CENTER_X - dim_total_w / 2;
    for (int i = 0; i < N_DIM; ++i) {
      const int x = dim_x0 + i * (OPT_BTN_W + OPT_GAP);
      if (panel_x >= x && panel_x < x + OPT_BTN_W) {
        ::robimon::services::config::set_uint(KEY_DIM_S, DIM_OPTS[i].secs);
        LOG_I(TAG, "idle dim -> %us", DIM_OPTS[i].secs);
        dirty_ = true;
        return;
      }
    }
  }

  // Sleep row
  if (canvas_y >= SLEEP_ROW_Y && canvas_y < SLEEP_ROW_Y + OPT_BTN_H) {
    const int sleep_total_w = N_SLEEP * OPT_BTN_W + (N_SLEEP - 1) * OPT_GAP;
    const int sleep_x0 = CENTER_X - sleep_total_w / 2;
    for (int i = 0; i < N_SLEEP; ++i) {
      const int x = sleep_x0 + i * (OPT_BTN_W + OPT_GAP);
      if (panel_x >= x && panel_x < x + OPT_BTN_W) {
        ::robimon::services::config::set_uint(KEY_SLEEP_S, SLEEP_OPTS[i].secs);
        LOG_I(TAG, "sleep -> %us", SLEEP_OPTS[i].secs);
        dirty_ = true;
        return;
      }
    }
  }
}

}  // namespace robimon::screens
