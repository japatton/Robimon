#include "screen_mgr.h"
#include "../app/log.h"

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
}

void on_tap(int panel_x, int panel_y) {
  Screen* a = active();
  if (a) a->on_tap(panel_x, panel_y);
}

}  // namespace robimon::ui::screen_mgr
