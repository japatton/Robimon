#include "screen_mgr.h"
#include "../hal/display.h"
#include "../app/log.h"

#include <Arduino_GFX_Library.h>

namespace robimon::ui::screen_mgr {

namespace {
constexpr const char* TAG = "scrn";
constexpr int MAX_SCREENS = 8;
constexpr int MAX_MODALS  = 4;

Screen* s_screens[MAX_SCREENS] = {0};
int     s_count   = 0;
int     s_current = 0;
bool    s_initial_appear_done = false;

Screen* s_modals[MAX_MODALS] = {0};
int     s_modal_count = 0;

Screen* active() {
  if (s_modal_count > 0) return s_modals[s_modal_count - 1];
  return (s_count > 0) ? s_screens[s_current] : nullptr;
}

// Carousel page indicator — N small circles along the bottom of the canvas.
// Drawn after the screen renders so it overlays whatever the screen drew at
// the same y. Pushed via flush_band so the rest of the panel isn't touched.
constexpr int DOT_BAND_Y       = 304;     // canvas-local y for the indicator band
constexpr int DOT_BAND_H       = 12;
constexpr int DOT_RADIUS       = 3;
constexpr int DOT_GAP          = 14;      // spacing between dot centers
constexpr uint16_t COLOR_FG      = 0x055F;   // bright cyan
constexpr uint16_t COLOR_DIM     = 0x01F6;   // dim cyan
constexpr uint16_t COLOR_BG      = 0x0000;
constexpr uint16_t DOT_COLOR_ON  = COLOR_FG;
constexpr uint16_t DOT_COLOR_OFF = COLOR_DIM;
constexpr uint16_t DOT_COLOR_BG  = COLOR_BG;

// Caption overlay (top of canvas, above any screen content). Rendered after
// the screen flushes so it persists across swipes and modals.
constexpr int CAPTION_BAND_Y     = 6;
constexpr int CAPTION_BAND_H     = 36;
constexpr int CAPTION_LINE_H     = 18;
constexpr int CAPTION_CHARS_LINE = 36;       // safe width @ size 2 on 466 px canvas
constexpr size_t CAPTION_MAX     = 64;
char     s_caption[CAPTION_MAX + 1] = {0};
uint32_t s_caption_until_ms = 0;

void draw_carousel_dots() {
  if (s_count <= 1 || s_modal_count > 0) return;
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  const int canvas_w = g->width();
  // Clear the band (overlapping screen content here is acceptable — see the
  // band-y comment in screen_mgr.h). The dots themselves take over the band.
  g->fillRect(0, DOT_BAND_Y, canvas_w, DOT_BAND_H, DOT_COLOR_BG);
  const int total_w = (s_count - 1) * DOT_GAP + 2 * DOT_RADIUS;
  const int start_x = (canvas_w - total_w) / 2 + DOT_RADIUS;
  const int y       = DOT_BAND_Y + DOT_BAND_H / 2;
  for (int i = 0; i < s_count; ++i) {
    const int x = start_x + i * DOT_GAP;
    if (i == s_current) {
      g->fillCircle(x, y, DOT_RADIUS, DOT_COLOR_ON);
    } else {
      g->drawCircle(x, y, DOT_RADIUS, DOT_COLOR_OFF);
    }
  }
  ::robimon::hal::display::flush_band(DOT_BAND_Y, DOT_BAND_H);
}

void draw_caption_overlay() {
  // Track the last-rendered caption so we can do a one-time clear when it
  // expires — otherwise stale text would linger forever (the screens'
  // clear_screen_area() leaves the overlay bands alone).
  static bool s_last_active = false;
  const bool active = (s_caption[0] != '\0' && millis() < s_caption_until_ms);

  if (!active) {
    if (s_last_active) {
      Arduino_GFX* g = ::robimon::hal::display::gfx();
      if (g) {
        g->fillRect(0, CAPTION_BAND_Y, g->width(), CAPTION_BAND_H, COLOR_BG);
        ::robimon::hal::display::flush_band(CAPTION_BAND_Y, CAPTION_BAND_H);
      }
      s_last_active = false;
    }
    return;
  }
  s_last_active = true;

  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (!g) return;
  g->fillRect(0, CAPTION_BAND_Y, g->width(), CAPTION_BAND_H, COLOR_BG);

  g->setTextSize(2);
  g->setTextColor(COLOR_FG);
  const int cx  = g->width() / 2;
  const int len = (int)strlen(s_caption);

  if (len <= CAPTION_CHARS_LINE) {
    g->setCursor(cx - len * 6, CAPTION_BAND_Y + CAPTION_LINE_H / 2);
    g->print(s_caption);
  } else {
    // Wrap on the nearest space within ~12 chars of the soft limit so we
    // don't break a word.
    int split = CAPTION_CHARS_LINE;
    for (int i = CAPTION_CHARS_LINE; i > CAPTION_CHARS_LINE - 12 && i > 0; --i) {
      if (s_caption[i] == ' ') { split = i; break; }
    }
    char line1[CAPTION_MAX + 1];
    memcpy(line1, s_caption, split);
    line1[split] = '\0';
    const char* line2 = (s_caption[split] == ' ') ? &s_caption[split + 1]
                                                  : &s_caption[split];
    const int l1n = (int)strlen(line1);
    const int l2n = (int)strlen(line2);
    g->setCursor(cx - l1n * 6, CAPTION_BAND_Y);
    g->print(line1);
    g->setCursor(cx - l2n * 6, CAPTION_BAND_Y + CAPTION_LINE_H);
    g->print(line2);
  }
  ::robimon::hal::display::flush_band(CAPTION_BAND_Y, CAPTION_BAND_H);
}

}  // namespace

void begin() {
  s_count = 0;
  s_current = 0;
  s_initial_appear_done = false;
  s_modal_count = 0;
}

void add_screen(Screen* s) {
  if (s_count >= MAX_SCREENS) {
    LOG_W(TAG, "screen list full; dropping %s", s ? s->name() : "?");
    return;
  }
  s_screens[s_count++] = s;
  LOG_I(TAG, "added screen[%d] %s", s_count - 1, s->name());
}

void set_index(int idx) {
  if (s_count == 0) return;
  if (s_modal_count > 0) return;   // can't swap carousel while modal is up
  if (idx < 0) idx = 0;
  if (idx >= s_count) idx = s_count - 1;
  if (idx == s_current && s_initial_appear_done) return;

  if (s_initial_appear_done && s_screens[s_current]) {
    s_screens[s_current]->on_disappear();
  }

  // Wipe the canvas before the new screen renders. The face renderer
  // intentionally leaves the caption + dot bands alone (to avoid 30-FPS
  // flicker against the overlays), so without this, anything the
  // previous screen drew at the top or bottom of the canvas — or
  // anywhere the new screen doesn't fully overdraw — survives the swap.
  // fillScreen is just a framebuffer memset (no QSPI), so this is cheap.
  // Active caption + dots will be re-rendered by their overlays this
  // same frame.
  Arduino_GFX* g = ::robimon::hal::display::gfx();
  if (g) g->fillScreen(COLOR_BG);

  s_current = idx;
  s_initial_appear_done = true;
  if (s_screens[s_current]) {
    s_screens[s_current]->on_appear();
    LOG_I(TAG, "screen -> %s", s_screens[s_current]->name());
  }
}

void next() { if (s_count > 0 && s_modal_count == 0) set_index((s_current + 1) % s_count); }
void prev() { if (s_count > 0 && s_modal_count == 0) set_index((s_current - 1 + s_count) % s_count); }

int  index()  { return s_current; }
int  count()  { return s_count; }
Screen* current() { return active(); }

void push_modal(Screen* s) {
  if (!s) return;
  if (s_modal_count >= MAX_MODALS) {
    LOG_W(TAG, "modal stack full; dropping %s", s->name());
    return;
  }
  // The screen below us hides for now.
  Screen* below = active();
  if (below) below->on_disappear();
  s_modals[s_modal_count++] = s;
  s->on_appear();
  LOG_I(TAG, "modal+ %s (depth=%d)", s->name(), s_modal_count);
}

void pop_modal() {
  if (s_modal_count == 0) return;
  Screen* top = s_modals[--s_modal_count];
  if (top) top->on_disappear();
  Screen* now_active = active();
  if (now_active) now_active->on_appear();
  LOG_I(TAG, "modal- %s (depth=%d)", top ? top->name() : "?", s_modal_count);
}

int modal_depth() { return s_modal_count; }

void update(uint32_t now_ms) {
  if (s_count == 0 && s_modal_count == 0) return;
  if (!s_initial_appear_done && s_count > 0) {
    s_initial_appear_done = true;
    if (s_screens[s_current]) s_screens[s_current]->on_appear();
  }
  Screen* a = active();
  if (a) a->update(now_ms);
  // Overlays after the screen renders — caption (top band) and carousel
  // dots (bottom band). Each is its own partial flush, well under 1/26th
  // of a full flush per overlay.
  draw_caption_overlay();
  draw_carousel_dots();
}

void set_caption(const char* text, uint32_t duration_ms) {
  if (!text || !text[0]) {
    s_caption[0] = '\0';
    s_caption_until_ms = 0;
    return;
  }
  strncpy(s_caption, text, CAPTION_MAX);
  s_caption[CAPTION_MAX] = '\0';
  s_caption_until_ms = millis() + duration_ms;
}

void on_tap(int panel_x, int panel_y) {
  Screen* a = active();
  if (a) a->on_tap(panel_x, panel_y);
}

}  // namespace robimon::ui::screen_mgr
