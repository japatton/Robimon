#include "alarm_edit_screen.h"
#include "../hal/display.h"
#include "../services/alarm_mgr.h"
#include "../face/face.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

AlarmEditScreen alarm_edit_screen;

namespace {
constexpr const char* TAG = "alm-ed";

constexpr uint16_t COLOR_BG  = 0x0000;
constexpr uint16_t COLOR_FG  = 0x055F;
constexpr uint16_t COLOR_DIM = 0x01F6;
constexpr uint16_t COLOR_OK  = 0x07E0;
constexpr uint16_t COLOR_ERR = 0xF800;

constexpr int CW       = 466;
constexpr int CENTER_X = CW / 2;

// Time row layout
constexpr int TIME_Y         = 50;
constexpr int TIME_BTN_W     = 32;
constexpr int TIME_BTN_H     = 32;
constexpr int TIME_DIGIT_W   = 60;          // approx visual width of "HH" at TextSize 4
constexpr int TIME_GAP       = 8;
constexpr int TIME_COLON_W   = 16;

// Days row — 7 day buttons + an "every day" pill on the right that
// disambiguates "every day" (mask=0) from "all 7 individually selected"
// (mask=0x7F). Tapping the pill always clears to mask=0.
constexpr int DAY_Y          = 110;
constexpr int DAY_BTN_W      = 32;
constexpr int DAY_BTN_H      = 32;
constexpr int DAY_GAP        = 6;
constexpr int DAY_ALL_W      = 60;            // "every" pill
constexpr int DAY_TOTAL      = 7 * DAY_BTN_W + 6 * DAY_GAP + DAY_GAP + DAY_ALL_W;
constexpr int DAY_X0         = (CW - DAY_TOTAL) / 2;
constexpr int DAY_ALL_X      = DAY_X0 + 7 * DAY_BTN_W + 6 * DAY_GAP + DAY_GAP;
constexpr char DAY_LETTERS[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };

// Expression row
constexpr int EXPR_Y         = 160;
constexpr int EXPR_BTN_W     = 36;
constexpr int EXPR_BTN_H     = 32;
constexpr int EXPR_BOX_W     = 200;

// Enabled toggle
constexpr int EN_Y           = 210;
constexpr int EN_W           = 130;
constexpr int EN_H           = 32;

// Bottom action row
constexpr int ACT_Y          = 270;
constexpr int ACT_H          = 36;
constexpr int ACT_W          = 96;

// face::Expression names — match the order in face/face.h.
const char* const EXPR_NAMES[] = {
  "neutral", "happy", "sad", "sleepy", "surprised",
  "angry", "thinking", "excited", "confused",
  "listening", "speaking",
};
constexpr int N_EXPRS = sizeof(EXPR_NAMES) / sizeof(EXPR_NAMES[0]);

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

// Time row x layout: [-H] HH [+H] : [-M] MM [+M]
void time_x(int* hm_x, int* hp_x, int* hh_x, int* mm_x, int* mp_x, int* mn_x, int* col_x) {
  const int total = 4 * TIME_BTN_W + 2 * TIME_DIGIT_W + TIME_COLON_W + 6 * TIME_GAP;
  int x = CENTER_X - total / 2;
  *hm_x = x;                    x += TIME_BTN_W   + TIME_GAP;
  *hh_x = x;                    x += TIME_DIGIT_W + TIME_GAP;
  *hp_x = x;                    x += TIME_BTN_W   + TIME_GAP;
  *col_x = x;                   x += TIME_COLON_W + TIME_GAP;
  *mn_x = x;                    x += TIME_BTN_W   + TIME_GAP;
  *mm_x = x;                    x += TIME_DIGIT_W + TIME_GAP;
  *mp_x = x;
}

}  // namespace

void AlarmEditScreen::on_appear() {
  dirty_ = true;
}

void AlarmEditScreen::update(uint32_t now_ms) {
  if (!dirty_ && (now_ms - last_render_ms_) < 200) return;
  dirty_ = false;
  last_render_ms_ = now_ms;
  render();
}

void AlarmEditScreen::render() {
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  draw_centered_text(g, 8, 2, COLOR_DIM,
                     edit_idx_ < 0 ? "new alarm" : "edit alarm");

  // ---- Time row ------------------------------------------------------------
  int hm_x, hp_x, hh_x, mm_x, mp_x, mn_x, col_x;
  time_x(&hm_x, &hp_x, &hh_x, &mm_x, &mp_x, &mn_x, &col_x);

  draw_button(g, hm_x, TIME_Y, TIME_BTN_W, TIME_BTN_H, COLOR_FG, "-");
  draw_button(g, hp_x, TIME_Y, TIME_BTN_W, TIME_BTN_H, COLOR_FG, "+");
  draw_button(g, mn_x, TIME_Y, TIME_BTN_W, TIME_BTN_H, COLOR_FG, "-");
  draw_button(g, mp_x, TIME_Y, TIME_BTN_W, TIME_BTN_H, COLOR_FG, "+");

  g->setTextSize(4);
  g->setTextColor(COLOR_FG);
  char b[4];
  snprintf(b, sizeof(b), "%02d", (int)staging_.hour);
  g->setCursor(hh_x + TIME_DIGIT_W / 2 - 24, TIME_Y);
  g->print(b);
  g->setCursor(col_x + TIME_COLON_W / 2 - 12, TIME_Y);
  g->print(":");
  snprintf(b, sizeof(b), "%02d", (int)staging_.minute);
  g->setCursor(mm_x + TIME_DIGIT_W / 2 - 24, TIME_Y);
  g->print(b);

  // ---- Days row ------------------------------------------------------------
  // "every day" mode (mask == 0) renders the 7 day buttons as outlines
  // (no fill) and lights up the "every" pill instead. That way the kid can
  // see the difference between "every day, always" and "I picked all 7
  // boxes individually" at a glance.
  const bool every_mode = (staging_.days_mask == 0);
  for (int i = 0; i < 7; ++i) {
    const int x = DAY_X0 + i * (DAY_BTN_W + DAY_GAP);
    const bool selected = (staging_.days_mask & (1 << i)) != 0;
    const uint16_t color = (selected || every_mode) ? COLOR_FG : COLOR_DIM;
    g->drawRoundRect(x, DAY_Y, DAY_BTN_W, DAY_BTN_H, 4, color);
    if (selected) {
      g->fillRoundRect(x + 4, DAY_Y + 4, DAY_BTN_W - 8, DAY_BTN_H - 8, 3, color);
      g->setTextColor(COLOR_BG);
    } else {
      g->setTextColor(color);
    }
    g->setTextSize(2);
    char letter[2] = { DAY_LETTERS[i], 0 };
    g->setCursor(x + DAY_BTN_W / 2 - 6, DAY_Y + DAY_BTN_H / 2 - 8);
    g->print(letter);
  }

  // "every day" pill at the right of the day row.
  const uint16_t all_color = every_mode ? COLOR_FG : COLOR_DIM;
  g->drawRoundRect(DAY_ALL_X,     DAY_Y,     DAY_ALL_W,     DAY_BTN_H,     6, all_color);
  g->drawRoundRect(DAY_ALL_X + 1, DAY_Y + 1, DAY_ALL_W - 2, DAY_BTN_H - 2, 5, all_color);
  if (every_mode) {
    g->fillRoundRect(DAY_ALL_X + 4, DAY_Y + 4, DAY_ALL_W - 8, DAY_BTN_H - 8, 4, all_color);
    g->setTextColor(COLOR_BG);
  } else {
    g->setTextColor(all_color);
  }
  g->setTextSize(2);
  const char* all_lbl = "every";
  g->setCursor(DAY_ALL_X + DAY_ALL_W / 2 - (int)strlen(all_lbl) * 6,
               DAY_Y + DAY_BTN_H / 2 - 8);
  g->print(all_lbl);

  // ---- Expression row ------------------------------------------------------
  const int expr_total = EXPR_BTN_W + 6 + EXPR_BOX_W + 6 + EXPR_BTN_W;
  const int expr_x0 = CENTER_X - expr_total / 2;
  draw_button(g, expr_x0, EXPR_Y, EXPR_BTN_W, EXPR_BTN_H, COLOR_FG, "<");
  g->drawRoundRect(expr_x0 + EXPR_BTN_W + 6, EXPR_Y, EXPR_BOX_W, EXPR_BTN_H, 6, COLOR_FG);
  g->drawRoundRect(expr_x0 + EXPR_BTN_W + 6 + 1, EXPR_Y + 1, EXPR_BOX_W - 2, EXPR_BTN_H - 2, 5, COLOR_FG);
  g->setTextColor(COLOR_FG);
  g->setTextSize(2);
  const char* en = (staging_.expression < N_EXPRS) ? EXPR_NAMES[staging_.expression] : "?";
  const int enw = (int)strlen(en) * 12;
  g->setCursor(expr_x0 + EXPR_BTN_W + 6 + EXPR_BOX_W / 2 - enw / 2, EXPR_Y + EXPR_BTN_H / 2 - 8);
  g->print(en);
  draw_button(g, expr_x0 + EXPR_BTN_W + 6 + EXPR_BOX_W + 6, EXPR_Y,
              EXPR_BTN_W, EXPR_BTN_H, COLOR_FG, ">");

  // ---- Enabled toggle ------------------------------------------------------
  const int en_x = CENTER_X - EN_W / 2;
  const uint16_t en_color = staging_.enabled ? COLOR_OK : COLOR_DIM;
  g->drawRoundRect(en_x, EN_Y, EN_W, EN_H, 6, en_color);
  g->drawRoundRect(en_x + 1, EN_Y + 1, EN_W - 2, EN_H - 2, 5, en_color);
  g->setTextColor(en_color);
  g->setTextSize(2);
  const char* lbl = staging_.enabled ? "ON" : "OFF";
  const int lw = (int)strlen(lbl) * 12;
  g->setCursor(CENTER_X - lw / 2, EN_Y + EN_H / 2 - 8);
  g->print(lbl);

  // ---- Bottom actions ------------------------------------------------------
  const int row_total = ACT_W * 3 + 2 * 12;
  int ax = CENTER_X - row_total / 2;
  draw_button(g, ax, ACT_Y, ACT_W, ACT_H, COLOR_OK,  "save");  ax += ACT_W + 12;
  // Show "delete" only when editing an existing alarm; otherwise "discard"
  draw_button(g, ax, ACT_Y, ACT_W, ACT_H, COLOR_ERR,
              edit_idx_ >= 0 ? "delete" : "discard");           ax += ACT_W + 12;
  draw_button(g, ax, ACT_Y, ACT_W, ACT_H, COLOR_DIM, "cancel");

  ::robimon::hal::display::flush();
}

void AlarmEditScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  // Time row
  if (canvas_y >= TIME_Y && canvas_y < TIME_Y + TIME_BTN_H) {
    int hm_x, hp_x, hh_x, mm_x, mp_x, mn_x, col_x;
    time_x(&hm_x, &hp_x, &hh_x, &mm_x, &mp_x, &mn_x, &col_x);
    if (panel_x >= hm_x && panel_x < hm_x + TIME_BTN_W) {
      staging_.hour = (staging_.hour + 23) % 24; dirty_ = true; return;
    }
    if (panel_x >= hp_x && panel_x < hp_x + TIME_BTN_W) {
      staging_.hour = (staging_.hour + 1) % 24; dirty_ = true; return;
    }
    if (panel_x >= mn_x && panel_x < mn_x + TIME_BTN_W) {
      staging_.minute = (staging_.minute + 59) % 60; dirty_ = true; return;
    }
    if (panel_x >= mp_x && panel_x < mp_x + TIME_BTN_W) {
      staging_.minute = (staging_.minute + 1) % 60; dirty_ = true; return;
    }
  }

  // Days row + "every day" pill
  if (canvas_y >= DAY_Y && canvas_y < DAY_Y + DAY_BTN_H) {
    // "every day" pill — tap clears mask back to 0 (every day).
    if (panel_x >= DAY_ALL_X && panel_x < DAY_ALL_X + DAY_ALL_W) {
      staging_.days_mask = 0;
      dirty_ = true;
      return;
    }
    for (int i = 0; i < 7; ++i) {
      const int x = DAY_X0 + i * (DAY_BTN_W + DAY_GAP);
      if (panel_x >= x && panel_x < x + DAY_BTN_W) {
        // From "every day" mode, a single-day tap means "only this day".
        if (staging_.days_mask == 0) {
          staging_.days_mask = (1 << i);
        } else {
          staging_.days_mask ^= (1 << i);
          // Clearing the last selected day falls back to "every day".
          if ((staging_.days_mask & 0x7F) == 0) staging_.days_mask = 0;
        }
        dirty_ = true;
        return;
      }
    }
  }

  // Expression row
  if (canvas_y >= EXPR_Y && canvas_y < EXPR_Y + EXPR_BTN_H) {
    const int expr_total = EXPR_BTN_W + 6 + EXPR_BOX_W + 6 + EXPR_BTN_W;
    const int expr_x0 = CENTER_X - expr_total / 2;
    const int prev_x = expr_x0;
    const int next_x = expr_x0 + EXPR_BTN_W + 6 + EXPR_BOX_W + 6;
    if (panel_x >= prev_x && panel_x < prev_x + EXPR_BTN_W) {
      staging_.expression = (staging_.expression + N_EXPRS - 1) % N_EXPRS;
      dirty_ = true; return;
    }
    if (panel_x >= next_x && panel_x < next_x + EXPR_BTN_W) {
      staging_.expression = (staging_.expression + 1) % N_EXPRS;
      dirty_ = true; return;
    }
  }

  // Enabled
  const int en_x = CENTER_X - EN_W / 2;
  if (panel_x >= en_x && panel_x < en_x + EN_W &&
      canvas_y >= EN_Y && canvas_y < EN_Y + EN_H) {
    staging_.enabled = !staging_.enabled;
    dirty_ = true;
    return;
  }

  // Bottom actions
  if (canvas_y >= ACT_Y && canvas_y < ACT_Y + ACT_H) {
    const int row_total = ACT_W * 3 + 2 * 12;
    int ax = CENTER_X - row_total / 2;
    // Save
    if (panel_x >= ax && panel_x < ax + ACT_W) {
      LOG_I(TAG, "save alarm idx=%d (%02d:%02d, mask=0x%02X, expr=%u, en=%d)",
            edit_idx_, staging_.hour, staging_.minute, staging_.days_mask,
            (unsigned)staging_.expression, (int)staging_.enabled);
      using namespace ::robimon::services;
      if (edit_idx_ >= 0) alarm_mgr::set(edit_idx_, staging_);
      else                alarm_mgr::add(staging_);
      ::robimon::ui::screen_mgr::pop_modal();
      return;
    }
    ax += ACT_W + 12;
    // Delete / discard
    if (panel_x >= ax && panel_x < ax + ACT_W) {
      if (edit_idx_ >= 0) {
        LOG_I(TAG, "delete alarm %d", edit_idx_);
        ::robimon::services::alarm_mgr::remove(edit_idx_);
      }
      ::robimon::ui::screen_mgr::pop_modal();
      return;
    }
    ax += ACT_W + 12;
    // Cancel
    if (panel_x >= ax && panel_x < ax + ACT_W) {
      ::robimon::ui::screen_mgr::pop_modal();
      return;
    }
  }
}

}  // namespace robimon::screens
