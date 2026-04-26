#include "pin_pad_screen.h"
#include "settings_menu_screen.h"
#include "../hal/display.h"
#include "../services/config_store.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens {

PinPadScreen pin_pad_screen;

namespace {
constexpr const char* TAG = "pin";

constexpr uint16_t COLOR_BG    = 0x0000;
constexpr uint16_t COLOR_FG    = 0x055F;   // cyan, matches face
constexpr uint16_t COLOR_DIM   = 0x01F6;
constexpr uint16_t COLOR_ERROR = 0xF800;   // red

// 3×4 button grid. The canvas is 466 wide × 320 tall. Vertical layout:
//   y=  0..40  title + masked PIN display
//   y= 75..295 four rows of buttons (60 px apart, 56 px diameter → 4 px gaps)
//   y=300..320 failed-attempts counter
// Horizontal: three columns at x=(110, 233, 356) — 123 px apart, plenty
// for 56 px buttons with thumb-sized hit targets.
struct Btn { char label; int cx; int cy; };
constexpr int BTN_R    = 28;          // 56 px button diameter
constexpr int BTN_HIT  = 32;          // hit-test radius (slightly larger than visual)
constexpr int COL_X[3] = { 110, 233, 356 };
constexpr int ROW_Y[4] = {  85, 145, 205, 265 };
constexpr Btn BUTTONS[] = {
  {'1', COL_X[0], ROW_Y[0]}, {'2', COL_X[1], ROW_Y[0]}, {'3', COL_X[2], ROW_Y[0]},
  {'4', COL_X[0], ROW_Y[1]}, {'5', COL_X[1], ROW_Y[1]}, {'6', COL_X[2], ROW_Y[1]},
  {'7', COL_X[0], ROW_Y[2]}, {'8', COL_X[1], ROW_Y[2]}, {'9', COL_X[2], ROW_Y[2]},
  {'<', COL_X[0], ROW_Y[3]}, {'0', COL_X[1], ROW_Y[3]}, {'X', COL_X[2], ROW_Y[3]},
};
constexpr int BTN_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);

constexpr uint32_t FLASH_MS = 350;
constexpr uint8_t  MAX_FAILS = 3;

}  // namespace

void PinPadScreen::on_appear() {
  entered_[0] = '\0';
  entered_len_ = 0;
  fail_count_ = 0;
  flash_until_ms_ = 0;
  flash_error_ = false;
  dirty_ = true;
}

void PinPadScreen::update(uint32_t now_ms) {
  // Auto-clear the result flash.
  if (flash_until_ms_ != 0 && now_ms >= flash_until_ms_) {
    flash_until_ms_ = 0;
    dirty_ = true;
  }
  if (!dirty_ && (now_ms - last_render_ms_) < 100) return;
  dirty_ = false;
  last_render_ms_ = now_ms;
  render();
}

void PinPadScreen::render() {
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;

  g->fillScreen(COLOR_BG);

  const int cx = g->width() / 2;
  const uint16_t accent = (flash_until_ms_ != 0)
      ? (flash_error_ ? COLOR_ERROR : COLOR_FG)
      : COLOR_FG;

  // Title at the very top.
  g->setTextColor(COLOR_DIM);
  g->setTextSize(2);
  const char* title = "enter PIN";
  const int t_w = (int)strlen(title) * 12;
  g->setCursor(cx - t_w / 2, 6);
  g->print(title);

  // Masked PIN display: 4 boxes, smaller and tighter than v1.
  constexpr int BOX_W = 24;
  constexpr int BOX_GAP = 12;
  const int total_w = 4 * BOX_W + 3 * BOX_GAP;
  const int x0 = cx - total_w / 2;
  const int box_y = 30;
  for (int i = 0; i < 4; ++i) {
    const int bx = x0 + i * (BOX_W + BOX_GAP);
    g->drawRect(bx, box_y, BOX_W, BOX_W, accent);
    g->drawRect(bx + 1, box_y + 1, BOX_W - 2, BOX_W - 2, accent);
    if (i < entered_len_) g->fillCircle(bx + BOX_W / 2, box_y + BOX_W / 2, 5, accent);
  }

  // Buttons: 56 px diameter, 4 px between rows, comfortable hit targets.
  g->setTextSize(3);
  for (int i = 0; i < BTN_COUNT; ++i) {
    const Btn& b = BUTTONS[i];
    g->drawCircle(b.cx, b.cy, BTN_R,     accent);
    g->drawCircle(b.cx, b.cy, BTN_R - 1, accent);
    g->setTextColor(accent);
    char s[2] = { b.label, 0 };
    const int char_w = 18;
    g->setCursor(b.cx - char_w / 2, b.cy - 12);
    if (b.label == '<') g->print("<");
    else if (b.label == 'X') g->print("X");
    else g->print(s);
  }

  // Failed-attempts counter (only if any fails)
  if (fail_count_ > 0) {
    g->setTextColor(COLOR_ERROR);
    g->setTextSize(1);
    char buf[24];
    snprintf(buf, sizeof(buf), "wrong %d/%d", fail_count_, MAX_FAILS);
    const int w = (int)strlen(buf) * 6;
    g->setCursor(cx - w / 2, g->height() - 12);
    g->print(buf);
  }

  ::robimon::hal::display::flush();
}

void PinPadScreen::on_tap(int panel_x, int panel_y) {
  // Convert panel y to canvas-local for hit testing.
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  for (int i = 0; i < BTN_COUNT; ++i) {
    const Btn& b = BUTTONS[i];
    const int dx = panel_x - b.cx;
    const int dy = canvas_y - b.cy;
    if (dx * dx + dy * dy > BTN_HIT * BTN_HIT) continue;

    if (b.label == '<') {
      // Backspace
      if (entered_len_ > 0) {
        entered_[--entered_len_] = '\0';
        dirty_ = true;
      }
      return;
    }
    if (b.label == 'X') {
      // Cancel — pop back to underlying screen.
      LOG_I(TAG, "pin entry cancelled");
      ::robimon::ui::screen_mgr::pop_modal();
      return;
    }
    // Digit
    if (entered_len_ < 4) {
      entered_[entered_len_++] = b.label;
      entered_[entered_len_]   = '\0';
      dirty_ = true;
      if (entered_len_ == 4) try_submit();
    }
    return;
  }
}

void PinPadScreen::try_submit() {
  if (::robimon::services::config::pin_check(entered_)) {
    LOG_I(TAG, "pin OK -> settings");
    flash_error_ = false;
    flash_until_ms_ = millis() + FLASH_MS;
    // Clear immediately so the next time we appear we start clean.
    entered_[0] = '\0';
    entered_len_ = 0;
    fail_count_ = 0;
    dirty_ = true;
    // Replace ourselves with the settings menu (pop self, push menu).
    ::robimon::ui::screen_mgr::pop_modal();
    ::robimon::ui::screen_mgr::push_modal(&settings_menu_screen);
    return;
  }

  fail_count_++;
  flash_error_ = true;
  flash_until_ms_ = millis() + FLASH_MS;
  entered_[0] = '\0';
  entered_len_ = 0;
  dirty_ = true;
  LOG_W(TAG, "pin wrong (%d/%d)", fail_count_, MAX_FAILS);

  if (fail_count_ >= MAX_FAILS) {
    LOG_W(TAG, "pin entry locked out");
    ::robimon::ui::screen_mgr::pop_modal();
  }
}

}  // namespace robimon::screens
