#include "light_control_screen.h"
#include "../hal/display.h"
#include "../services/ha_client.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

LightControlScreen light_control_screen;

namespace {
constexpr const char* TAG = "light";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_OFF = 0xF800;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;
constexpr int BUTTON_H = 32;
constexpr int BACK_W   = 96;

// On/off button
constexpr int ONOFF_W = 200;
constexpr int ONOFF_H = 50;
constexpr int ONOFF_Y = 44;

// Slider rows
constexpr int LABEL_OFFSET = 18;     // label sits this many px above its slider row
// Bumped from 44×32 to 56×40 per the kid-UX review — child finger pads are
// ~2–3 cm at the fingertip; the prior 44 px width caused drift onto the
// slider bar between presses.
constexpr int SLIDER_BTN_W = 56;
constexpr int SLIDER_BTN_H = 40;
constexpr int SLIDER_BAR_H = 22;
constexpr int SLIDER_BAR_X = 86;
constexpr int SLIDER_BAR_W = CW - 2 * SLIDER_BAR_X;
constexpr int SLIDER_MINUS_X = SLIDER_BAR_X - SLIDER_BTN_W - 6;
constexpr int SLIDER_PLUS_X  = SLIDER_BAR_X + SLIDER_BAR_W + 6;
constexpr int BRIGHTNESS_Y = 130;
constexpr int COLORTEMP_Y  = 200;

// Step sizes — how much each +/- tap changes the value.
constexpr int BRIGHTNESS_STEP = 25;   // ~10 increments across 0..255
constexpr int KELVIN_STEP     = 500;

void draw_centered_text(Arduino_GFX* g, int y, int sz, uint16_t color, const char* s) {
  g->setTextColor(color);
  g->setTextSize(sz);
  g->setCursor(CENTER_X - (int)strlen(s) * 6 * sz / 2, y);
  g->print(s);
}
void draw_button(Arduino_GFX* g, int x, int y, int w, int h, uint16_t color, const char* label) {
  g->drawRoundRect(x,     y,     w,     h,     6, color);
  g->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 5, color);
  g->setTextColor(color);
  g->setTextSize(2);
  g->setCursor(x + w / 2 - (int)strlen(label) * 12 / 2, y + h / 2 - 8);
  g->print(label);
}
void draw_slider_row(Arduino_GFX* g, int y, const char* label, int value, int max_val, const char* unit) {
  // Label + value above the slider row
  g->setTextSize(2);
  g->setTextColor(COLOR_DIM);
  g->setCursor(SLIDER_MINUS_X, y - LABEL_OFFSET);
  g->print(label);
  char buf[20];
  if (unit && *unit) snprintf(buf, sizeof(buf), "%d%s", value, unit);
  else               snprintf(buf, sizeof(buf), "%d/%d", value, max_val);
  const int vw = (int)strlen(buf) * 12;
  g->setTextColor(COLOR_FG);
  g->setCursor(SLIDER_PLUS_X + SLIDER_BTN_W - vw, y - LABEL_OFFSET);
  g->print(buf);

  // -/+ buttons + bar
  draw_button(g, SLIDER_MINUS_X, y, SLIDER_BTN_W, SLIDER_BTN_H, COLOR_FG, "-");
  draw_button(g, SLIDER_PLUS_X,  y, SLIDER_BTN_W, SLIDER_BTN_H, COLOR_FG, "+");
  // Bar centered vertically in the row
  const int bar_y = y + SLIDER_BTN_H / 2 - SLIDER_BAR_H / 2;
  g->drawRoundRect(SLIDER_BAR_X,     bar_y,     SLIDER_BAR_W,     SLIDER_BAR_H,     4, COLOR_FG);
  g->drawRoundRect(SLIDER_BAR_X + 1, bar_y + 1, SLIDER_BAR_W - 2, SLIDER_BAR_H - 2, 3, COLOR_FG);
  if (max_val > 0 && value > 0) {
    int filled = (int)((int64_t)(value) * (SLIDER_BAR_W - 6) / max_val);
    if (filled < 0) filled = 0;
    if (filled > SLIDER_BAR_W - 6) filled = SLIDER_BAR_W - 6;
    g->fillRoundRect(SLIDER_BAR_X + 3, bar_y + 3, filled, SLIDER_BAR_H - 6, 2, COLOR_FG);
  }
}

}  // namespace

void LightControlScreen::on_appear() {
  ::robimon::services::ha_client::track_entity(entity_id_);
  dirty_ = true;
}

void LightControlScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  using namespace ::robimon::services;

  const auto* st = ha_client::get_state(entity_id_);
  const char* name = (st && st->friendly_name[0]) ? st->friendly_name : entity_id_;
  const bool is_on = (st && strcmp(st->state, "on") == 0);

  // Title
  g->setTextColor(COLOR_FG);
  g->setTextSize(3);
  g->setCursor(CENTER_X - (int)strlen(name) * 18 / 2, 8);
  g->print(name);

  // ON/OFF big button
  draw_button(g, CENTER_X - ONOFF_W / 2, ONOFF_Y, ONOFF_W, ONOFF_H,
              is_on ? COLOR_OK : COLOR_OFF,
              is_on ? "ON" : "OFF");

  // Brightness slider (only meaningful if light is on or supports brightness)
  const int b = (st && st->brightness >= 0) ? st->brightness : 0;
  draw_slider_row(g, BRIGHTNESS_Y, "brightness", b, 255, "");

  // Color temp slider (only if entity reports kelvin range)
  if (st && st->min_color_temp_kelvin > 0 && st->max_color_temp_kelvin > st->min_color_temp_kelvin) {
    const int k = (st->color_temp_kelvin > 0)
                  ? st->color_temp_kelvin
                  : st->min_color_temp_kelvin;
    char label[24];
    snprintf(label, sizeof(label), "color temp");
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%dK", k);
    // Use draw_slider_row with kelvin-relative max so the bar fill maps to range.
    const int relative = k - st->min_color_temp_kelvin;
    const int range    = st->max_color_temp_kelvin - st->min_color_temp_kelvin;
    draw_slider_row(g, COLORTEMP_Y, "color temp", relative, range, "");
    // Override the value text we just drew with absolute K value.
    g->setTextSize(2);
    g->setTextColor(COLOR_FG);
    const int vw = (int)strlen(vbuf) * 12;
    // Clear the area where the previous value text lives, then redraw.
    g->fillRect(SLIDER_PLUS_X + SLIDER_BTN_W - 80, COLORTEMP_Y - LABEL_OFFSET, 80, 16, COLOR_BG);
    g->setCursor(SLIDER_PLUS_X + SLIDER_BTN_W - vw, COLORTEMP_Y - LABEL_OFFSET);
    g->print(vbuf);
  } else {
    // Entity doesn't support color temp — show a dim hint where the slider would be.
    g->setTextColor(COLOR_DIM);
    g->setTextSize(2);
    g->setCursor(SLIDER_MINUS_X, COLORTEMP_Y - LABEL_OFFSET);
    g->print("color temp not supported");
  }

  // Back at bottom
  draw_button(g, CENTER_X - BACK_W / 2, 320 - BUTTON_H - 8,
              BACK_W, BUTTON_H, COLOR_DIM, "back");

  ::robimon::hal::display::flush();
}

void LightControlScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  using namespace ::robimon::services;

  // Back
  const int back_y = 320 - BUTTON_H - 8;
  if (panel_x >= CENTER_X - BACK_W / 2 && panel_x < CENTER_X + BACK_W / 2 &&
      canvas_y >= back_y && canvas_y < back_y + BUTTON_H) {
    ::robimon::ui::screen_mgr::pop_modal();
    return;
  }

  // ON/OFF
  if (panel_x >= CENTER_X - ONOFF_W / 2 && panel_x < CENTER_X + ONOFF_W / 2 &&
      canvas_y >= ONOFF_Y && canvas_y < ONOFF_Y + ONOFF_H) {
    const auto* st = ha_client::get_state(entity_id_);
    const bool is_on = (st && strcmp(st->state, "on") == 0);
    if (is_on) ha_client::light_turn_off(entity_id_);
    else       ha_client::light_turn_on(entity_id_);
    dirty_ = true;
    return;
  }

  // Brightness -/+
  if (canvas_y >= BRIGHTNESS_Y && canvas_y < BRIGHTNESS_Y + SLIDER_BTN_H) {
    const auto* st = ha_client::get_state(entity_id_);
    int b = (st && st->brightness >= 0) ? st->brightness : 0;
    if (panel_x >= SLIDER_MINUS_X && panel_x < SLIDER_MINUS_X + SLIDER_BTN_W) {
      b -= BRIGHTNESS_STEP; if (b < 0) b = 0;
      ha_client::light_turn_on(entity_id_, b);
      dirty_ = true; return;
    }
    if (panel_x >= SLIDER_PLUS_X && panel_x < SLIDER_PLUS_X + SLIDER_BTN_W) {
      b += BRIGHTNESS_STEP; if (b > 255) b = 255;
      ha_client::light_turn_on(entity_id_, b);
      dirty_ = true; return;
    }
  }

  // Color temp -/+
  if (canvas_y >= COLORTEMP_Y && canvas_y < COLORTEMP_Y + SLIDER_BTN_H) {
    const auto* st = ha_client::get_state(entity_id_);
    if (!st || st->min_color_temp_kelvin <= 0) return;
    int k = (st->color_temp_kelvin > 0) ? st->color_temp_kelvin : st->min_color_temp_kelvin;
    if (panel_x >= SLIDER_MINUS_X && panel_x < SLIDER_MINUS_X + SLIDER_BTN_W) {
      k -= KELVIN_STEP;
      if (k < st->min_color_temp_kelvin) k = st->min_color_temp_kelvin;
      ha_client::light_turn_on(entity_id_, /*brightness=*/-1, k);
      dirty_ = true; return;
    }
    if (panel_x >= SLIDER_PLUS_X && panel_x < SLIDER_PLUS_X + SLIDER_BTN_W) {
      k += KELVIN_STEP;
      if (k > st->max_color_temp_kelvin) k = st->max_color_temp_kelvin;
      ha_client::light_turn_on(entity_id_, /*brightness=*/-1, k);
      dirty_ = true; return;
    }
  }
}

}  // namespace robimon::screens
