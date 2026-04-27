// Screen manager — holds an ordered list of swipeable screens AND a small
// modal stack that lives on top of them. Settings, PIN entry, etc. push
// onto the modal stack and block swipes; popping all modals returns to the
// underlying carousel. Snap transitions for the carousel (slide animation
// can land later if it bothers).

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
Screen* current();   // top of modal stack if any, else carousel current

// ---- Modal stack ----------------------------------------------------------
// Modal screens live on top of the carousel. While at least one modal is
// pushed, swipes are ignored (no carousel nav) and on_tap goes to the top
// modal. Pushing/popping triggers on_appear/on_disappear like normal.
void push_modal(Screen* s);
void pop_modal();
int  modal_depth();

// Per-frame entry point — called from main.cpp::loop().
void update(uint32_t now_ms);

// Forward a tap to the current screen (modal top if any, else carousel).
void on_tap(int panel_x, int panel_y);

// Caption shown at the top of the canvas for `duration_ms`. Rendered as a
// global overlay (above whatever the current screen drew) so prompts
// persist across swipes — useful for the voice-flow "you said: ..."
// callout and the first-boot gesture tutorial.
void set_caption(const char* text, uint32_t duration_ms = 4000);

}  // namespace screen_mgr
}  // namespace robimon::ui
