// Screen manager — holds an ordered list of screens, tracks the current one,
// and routes input + per-frame updates to it. Stage E uses snap transitions
// (no slide animation yet) — swipe left/right just changes the active index.
//
// Settings is intentionally NOT a screen in this rotation; it's reached via
// long-press + PIN and lives outside the swipe path.

#pragma once

#include <stdint.h>

namespace robimon::ui {

class Screen {
 public:
  virtual ~Screen() = default;

  // Called when the screen becomes visible (after a swipe or initial set).
  virtual void on_appear() {}
  // Called when the screen is being replaced.
  virtual void on_disappear() {}
  // Called every loop iteration; the screen is responsible for any
  // per-frame work and for calling display::flush() (or letting whoever
  // it owns do that).
  virtual void update(uint32_t now_ms) = 0;
  // Tap at panel coords (0..LCD_WIDTH × 0..LCD_HEIGHT).
  virtual void on_tap(int panel_x, int panel_y) {}

  // Short identifier used in logs and (eventually) the page indicator.
  virtual const char* name() const = 0;
};

namespace screen_mgr {

void begin();

// Append a screen to the end of the rotation. The first screen added
// becomes the initial active screen.
void add_screen(Screen* s);

// Cycle to the next / previous screen with a snap transition.
void next();
void prev();

// Direct jump.
void set_index(int idx);

int  index();
int  count();
Screen* current();

// Per-frame entry point — called from main.cpp::loop().
void update(uint32_t now_ms);

// Forward a tap to the current screen.
void on_tap(int panel_x, int panel_y);

}  // namespace screen_mgr
}  // namespace robimon::ui
