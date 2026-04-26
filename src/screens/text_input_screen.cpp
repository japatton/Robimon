#include "text_input_screen.h"
#include "../hal/display.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

namespace robimon::screens::text_input {

namespace {
constexpr const char* TAG = "txt";

constexpr uint16_t COLOR_BG     = 0x0000;
constexpr uint16_t COLOR_FG     = 0x055F;   // matches face cyan
constexpr uint16_t COLOR_DIM    = 0x01F6;
constexpr uint16_t COLOR_OK     = 0x07E0;   // green for "done"
constexpr uint16_t COLOR_ERR    = 0xF800;   // red for cancel

// ---- Keyboard layout -------------------------------------------------------
// 4 rows. Letters page = letters; symbols page = numbers + punctuation.
// Special key codes (signed, since some are >127):
constexpr char K_SHIFT = '\x01';
constexpr char K_BS    = '\x02';
constexpr char K_TOG   = '\x03';   // toggle between letters and symbols
constexpr char K_SPACE = ' ';
constexpr char K_DONE  = '\x04';
constexpr char K_NONE  = '\0';

// Layouts. Row 4 is implicit (123/ABC, space, done).
const char* const LETTERS_LOWER[3] = {
  "qwertyuiop",
  "asdfghjkl",
  "\x01zxcvbnm\x02",   // ⇧ z x c v b n m ⌫
};
const char* const LETTERS_UPPER[3] = {
  "QWERTYUIOP",
  "ASDFGHJKL",
  "\x01ZXCVBNM\x02",
};
const char* const SYMBOLS[3] = {
  "1234567890",
  "-/:;()$&@\"",
  ".,?!'_+=\x02",      // . , ? ! ' _ + = ⌫
};

// Geometry — uniform key size; rows centered horizontally. Slightly larger
// hit targets (40×38 vs the original 40×32) reduce mistaps without forcing
// a layout reshuffle.
constexpr int KEY_W    = 40;
constexpr int KEY_H    = 38;
constexpr int KEY_GAP  = 3;
constexpr int KBD_TOP_Y = 108;        // canvas-local
constexpr int ROW_DY   = KEY_H + KEY_GAP;

constexpr int CANCEL_W = 60, CANCEL_H = 28;
constexpr int EYE_W    = 60, EYE_H    = 28;

constexpr int FIELD_X = 24, FIELD_Y = 36, FIELD_W = 466 - 48, FIELD_H = 36;

constexpr size_t MAX_BUF = 96;

enum class Mode : uint8_t { LOWER, UPPER, SYMBOLS };

class TextInputScreen : public ::robimon::ui::Screen {
 public:
  void on_appear() override   { dirty_ = true; }
  void on_disappear() override {}
  void update(uint32_t now_ms) override {
    if (!dirty_ && (now_ms - last_render_ms_) < 100) return;
    dirty_ = false;
    last_render_ms_ = now_ms;
    render();
  }
  void on_tap(int panel_x, int panel_y) override;
  const char* name() const override { return "txt-in"; }

  // Configured before push.
  char       title_[48]   = {0};
  char       buf_[MAX_BUF] = {0};
  size_t     max_len_     = MAX_BUF - 1;
  bool       masked_      = false;
  bool       show_unmasked_ = false;
  Mode       mode_        = Mode::LOWER;
  Callback   cb_          = nullptr;

 private:
  bool      dirty_         = true;
  uint32_t  last_render_ms_ = 0;

  void render();
  void layout_row(int row_idx, int* out_x_start, int* out_count);
  void key_at_index(int row, int col, char* out_label);
  bool handle_key(char k);
};

TextInputScreen s_screen;

// Helper: returns the row's keys (string + length).
const char* row_chars(Mode mode, int row, int* len) {
  const char* const* layout =
      (mode == Mode::SYMBOLS) ? SYMBOLS :
      (mode == Mode::UPPER  ) ? LETTERS_UPPER : LETTERS_LOWER;
  if (row < 0 || row >= 3) { *len = 0; return ""; }
  const char* s = layout[row];
  *len = (int)strlen(s);
  return s;
}

void TextInputScreen::layout_row(int row_idx, int* out_x_start, int* out_count) {
  int n; row_chars(mode_, row_idx, &n);
  const int row_w = n * KEY_W + (n - 1) * KEY_GAP;
  *out_x_start = (466 - row_w) / 2;
  *out_count = n;
}

void TextInputScreen::render() {
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillScreen(COLOR_BG);

  // Title
  g->setTextColor(COLOR_DIM);
  g->setTextSize(2);
  g->setCursor(20, 8);
  g->print(title_);

  // Cancel + show/hide buttons (top right)
  const int cancel_x = 466 - CANCEL_W - 12;
  const int cancel_y = 4;
  g->drawRoundRect(cancel_x,     cancel_y,     CANCEL_W,     CANCEL_H,     6, COLOR_ERR);
  g->drawRoundRect(cancel_x + 1, cancel_y + 1, CANCEL_W - 2, CANCEL_H - 2, 5, COLOR_ERR);
  g->setTextColor(COLOR_ERR);
  g->setCursor(cancel_x + CANCEL_W / 2 - 18, cancel_y + CANCEL_H / 2 - 8);
  g->print("X");

  if (masked_) {
    const int eye_x = cancel_x - EYE_W - 8;
    g->drawRoundRect(eye_x,     cancel_y,     EYE_W,     EYE_H,     6, COLOR_DIM);
    g->drawRoundRect(eye_x + 1, cancel_y + 1, EYE_W - 2, EYE_H - 2, 5, COLOR_DIM);
    g->setTextColor(COLOR_DIM);
    g->setCursor(eye_x + 8, cancel_y + EYE_H / 2 - 8);
    g->print(show_unmasked_ ? "hide" : "show");
  }

  // Input field
  g->drawRoundRect(FIELD_X,     FIELD_Y,     FIELD_W,     FIELD_H,     6, COLOR_FG);
  g->drawRoundRect(FIELD_X + 1, FIELD_Y + 1, FIELD_W - 2, FIELD_H - 2, 5, COLOR_FG);
  g->setTextColor(COLOR_FG);
  g->setTextSize(2);
  g->setCursor(FIELD_X + 8, FIELD_Y + FIELD_H / 2 - 8);
  if (masked_ && !show_unmasked_) {
    char dots[MAX_BUF];
    const size_t n = strlen(buf_);
    for (size_t i = 0; i < n && i < sizeof(dots) - 1; ++i) dots[i] = '*';
    dots[n < sizeof(dots) - 1 ? n : sizeof(dots) - 1] = '\0';
    g->print(dots);
  } else {
    g->print(buf_);
  }

  // Keyboard rows 0..2
  g->setTextSize(2);
  for (int row = 0; row < 3; ++row) {
    int n; const char* keys = row_chars(mode_, row, &n);
    int x0; int cnt; layout_row(row, &x0, &cnt);
    const int y = KBD_TOP_Y + row * ROW_DY;
    for (int i = 0; i < n; ++i) {
      const int x = x0 + i * (KEY_W + KEY_GAP);
      g->drawRoundRect(x,     y,     KEY_W,     KEY_H,     5, COLOR_FG);
      g->drawRoundRect(x + 1, y + 1, KEY_W - 2, KEY_H - 2, 4, COLOR_FG);
      g->setTextColor(COLOR_FG);
      const char k = keys[i];
      const char* label = nullptr;
      char single[2] = { k, 0 };
      if      (k == K_SHIFT) label = (mode_ == Mode::UPPER) ? "v" : "^";
      else if (k == K_BS)    label = "<";
      else                   label = single;
      const int lw = (int)strlen(label) * 12;
      g->setCursor(x + KEY_W / 2 - lw / 2, y + KEY_H / 2 - 8);
      g->print(label);
    }
  }

  // Row 3: [123/ABC] [space] [done]
  const int row_y = KBD_TOP_Y + 3 * ROW_DY;
  const int b1_w = 60;
  const int b3_w = 80;
  const int b2_w = 466 - b1_w - b3_w - 4 * KEY_GAP - 24;
  const int b1_x = (466 - (b1_w + KEY_GAP + b2_w + KEY_GAP + b3_w)) / 2;
  const int b2_x = b1_x + b1_w + KEY_GAP;
  const int b3_x = b2_x + b2_w + KEY_GAP;

  g->drawRoundRect(b1_x, row_y, b1_w, KEY_H, 5, COLOR_FG);
  g->setTextColor(COLOR_FG);
  g->setCursor(b1_x + b1_w / 2 - 18, row_y + KEY_H / 2 - 8);
  g->print(mode_ == Mode::SYMBOLS ? "ABC" : "123");

  g->drawRoundRect(b2_x, row_y, b2_w, KEY_H, 5, COLOR_FG);
  g->setCursor(b2_x + b2_w / 2 - 30, row_y + KEY_H / 2 - 8);
  g->print("space");

  g->drawRoundRect(b3_x, row_y, b3_w, KEY_H, 5, COLOR_OK);
  g->drawRoundRect(b3_x + 1, row_y + 1, b3_w - 2, KEY_H - 2, 4, COLOR_OK);
  g->setTextColor(COLOR_OK);
  g->setCursor(b3_x + b3_w / 2 - 24, row_y + KEY_H / 2 - 8);
  g->print("done");

  ::robimon::hal::display::flush();
}

bool TextInputScreen::handle_key(char k) {
  if (k == K_SHIFT) {
    mode_ = (mode_ == Mode::UPPER) ? Mode::LOWER : Mode::UPPER;
    return true;
  }
  if (k == K_TOG) {
    mode_ = (mode_ == Mode::SYMBOLS) ? Mode::LOWER : Mode::SYMBOLS;
    return true;
  }
  if (k == K_BS) {
    const size_t n = strlen(buf_);
    if (n > 0) buf_[n - 1] = '\0';
    return true;
  }
  if (k == K_DONE) {
    Callback cb = cb_;
    char snapshot[MAX_BUF];
    strncpy(snapshot, buf_, sizeof(snapshot)); snapshot[sizeof(snapshot)-1] = '\0';
    ::robimon::ui::screen_mgr::pop_modal();
    if (cb) cb(snapshot, /*cancelled=*/false);
    return false;   // don't redraw — we're popping
  }
  if (k == K_NONE) return false;
  // Printable
  const size_t n = strlen(buf_);
  if (n < max_len_ && n + 1 < sizeof(buf_)) {
    buf_[n] = k;
    buf_[n + 1] = '\0';
    // Auto-deshift after a single shifted letter.
    if (mode_ == Mode::UPPER) mode_ = Mode::LOWER;
  }
  return true;
}

void TextInputScreen::on_tap(int panel_x, int panel_y) {
  const int canvas_y = panel_y - ::robimon::hal::display::canvas_panel_y_offset();

  // Cancel button
  const int cancel_x = 466 - CANCEL_W - 12;
  if (panel_x >= cancel_x && panel_x < cancel_x + CANCEL_W &&
      canvas_y >= 4 && canvas_y < 4 + CANCEL_H) {
    Callback cb = cb_;
    ::robimon::ui::screen_mgr::pop_modal();
    if (cb) cb(buf_, /*cancelled=*/true);
    return;
  }
  // Show/hide
  if (masked_) {
    const int eye_x = cancel_x - EYE_W - 8;
    if (panel_x >= eye_x && panel_x < eye_x + EYE_W &&
        canvas_y >= 4 && canvas_y < 4 + EYE_H) {
      show_unmasked_ = !show_unmasked_;
      dirty_ = true;
      return;
    }
  }
  // Keyboard rows 0..2
  for (int row = 0; row < 3; ++row) {
    int n; const char* keys = row_chars(mode_, row, &n);
    int x0; int cnt; layout_row(row, &x0, &cnt);
    const int y = KBD_TOP_Y + row * ROW_DY;
    if (canvas_y < y || canvas_y >= y + KEY_H) continue;
    for (int i = 0; i < n; ++i) {
      const int x = x0 + i * (KEY_W + KEY_GAP);
      if (panel_x >= x && panel_x < x + KEY_W) {
        if (handle_key(keys[i])) dirty_ = true;
        return;
      }
    }
  }
  // Row 3 controls
  const int row_y = KBD_TOP_Y + 3 * ROW_DY;
  if (canvas_y >= row_y && canvas_y < row_y + KEY_H) {
    const int b1_w = 60;
    const int b3_w = 80;
    const int b2_w = 466 - b1_w - b3_w - 4 * KEY_GAP - 24;
    const int b1_x = (466 - (b1_w + KEY_GAP + b2_w + KEY_GAP + b3_w)) / 2;
    const int b2_x = b1_x + b1_w + KEY_GAP;
    const int b3_x = b2_x + b2_w + KEY_GAP;
    if      (panel_x >= b1_x && panel_x < b1_x + b1_w) { handle_key(K_TOG);   dirty_ = true; }
    else if (panel_x >= b2_x && panel_x < b2_x + b2_w) { handle_key(K_SPACE); dirty_ = true; }
    else if (panel_x >= b3_x && panel_x < b3_x + b3_w) { handle_key(K_DONE);  /* pops self */ }
  }
}

}  // namespace

void show(const char* title, const char* initial, bool masked,
          size_t max_len, Callback cb) {
  if (title)   { strncpy(s_screen.title_, title, sizeof(s_screen.title_) - 1); s_screen.title_[sizeof(s_screen.title_) - 1] = '\0'; }
  else         { s_screen.title_[0] = '\0'; }
  if (initial) { strncpy(s_screen.buf_, initial, sizeof(s_screen.buf_) - 1); s_screen.buf_[sizeof(s_screen.buf_) - 1] = '\0'; }
  else         { s_screen.buf_[0] = '\0'; }
  s_screen.masked_ = masked;
  s_screen.show_unmasked_ = false;
  s_screen.max_len_ = (max_len > MAX_BUF - 1) ? (MAX_BUF - 1) : max_len;
  s_screen.mode_   = Mode::LOWER;
  s_screen.cb_     = cb;
  ::robimon::ui::screen_mgr::push_modal(&s_screen);
  LOG_I(TAG, "text input opened (title='%s', masked=%d)", s_screen.title_, (int)masked);
}

}  // namespace robimon::screens::text_input
