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
constexpr uint16_t DOT_COLOR_ON  = 0x055F;   // bright cyan
constexpr uint16_t DOT_COLOR_OFF = 0x01F6;   // dim cyan
constexpr uint16_t DOT_COLOR_BG  = 0x0000;

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
  // Overlay carousel page dots after the screen renders. This is a 12-px
  // band with its own partial flush, so it costs ~1/26 of a full flush.
  draw_carousel_dots();
}

void on_tap(int panel_x, int panel_y) {
  Screen* a = active();
  if (a) a->on_tap(panel_x, panel_y);
}

}  // namespace robimon::ui::screen_mgr
