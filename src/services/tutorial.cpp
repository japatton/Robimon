#include "tutorial.h"
#include "config_store.h"
#include "../ui/screen_mgr.h"
#include "../face/face.h"
#include "../app/log.h"

#include <Arduino.h>

namespace robimon::services::tutorial {

namespace {
constexpr const char* TAG = "tutorial";
constexpr const char* KEY = "tut_done";

// Captions live until overwritten or until the long-poll duration expires.
// 10 minutes is the practical "abandoned" threshold — past that the kid
// has wandered off and we'd rather drop the prompt than have it hang on
// screen forever.
constexpr uint32_t CAPTION_HOLD_MS = 10UL * 60UL * 1000UL;

enum class Step : uint8_t {
  WELCOME,            // auto-advances after WELCOME_MS
  AWAIT_MOUTH_TAP,
  AWAIT_SWIPE,
  AWAIT_LONG_PRESS,
  CELEBRATE,          // auto-finishes after CELEBRATE_MS
  DONE,
};

constexpr uint32_t WELCOME_MS   = 2200;
constexpr uint32_t CELEBRATE_MS = 2800;

Step     s_step             = Step::DONE;
uint32_t s_step_started_ms  = 0;
bool     s_active           = false;

void set_caption(const char* text) {
  ::robimon::ui::screen_mgr::set_caption(text, CAPTION_HOLD_MS);
}

void enter(Step next, uint32_t now_ms) {
  s_step            = next;
  s_step_started_ms = now_ms;
  switch (next) {
    case Step::WELCOME:
      set_caption("hi! I'm Robimon");
      face::set_expression(face::Expression::HAPPY, 250);
      LOG_I(TAG, "step -> WELCOME");
      break;
    case Step::AWAIT_MOUTH_TAP:
      set_caption("tap my mouth to talk!");
      face::set_expression(face::Expression::NEUTRAL, 350);
      LOG_I(TAG, "step -> AWAIT_MOUTH_TAP");
      break;
    case Step::AWAIT_SWIPE:
      set_caption("now swipe sideways!");
      LOG_I(TAG, "step -> AWAIT_SWIPE");
      break;
    case Step::AWAIT_LONG_PRESS:
      set_caption("hold the screen for grown-ups");
      LOG_I(TAG, "step -> AWAIT_LONG_PRESS");
      break;
    case Step::CELEBRATE:
      set_caption("you got it! let's play");
      face::set_expression(face::Expression::EXCITED, 250);
      LOG_I(TAG, "step -> CELEBRATE");
      break;
    case Step::DONE:
      ::robimon::ui::screen_mgr::set_caption("", 0);
      s_active = false;
      config::set_uint(KEY, 1);
      LOG_I(TAG, "tutorial complete");
      break;
  }
}

}  // namespace

bool begin() {
  if (config::get_uint(KEY, 0) != 0) {
    s_active = false;
    s_step   = Step::DONE;
    return false;
  }
  s_active = true;
  enter(Step::WELCOME, millis());
  return true;
}

bool is_active() { return s_active; }

void update(uint32_t now_ms) {
  if (!s_active) return;

  // Auto-advance for time-based steps.
  if (s_step == Step::WELCOME && (now_ms - s_step_started_ms) >= WELCOME_MS) {
    enter(Step::AWAIT_MOUTH_TAP, now_ms);
    return;
  }
  if (s_step == Step::CELEBRATE && (now_ms - s_step_started_ms) >= CELEBRATE_MS) {
    enter(Step::DONE, now_ms);
    return;
  }

  // Refresh the caption near the end of the hold window so it never
  // disappears while the kid is still figuring out the gesture.
  if (s_step != Step::DONE && (now_ms - s_step_started_ms) > CAPTION_HOLD_MS - 30000UL) {
    enter(s_step, now_ms);
  }
}

void on_mouth_tap() {
  if (s_active && s_step == Step::AWAIT_MOUTH_TAP) {
    enter(Step::AWAIT_SWIPE, millis());
  }
}

void on_swipe() {
  if (s_active && s_step == Step::AWAIT_SWIPE) {
    enter(Step::AWAIT_LONG_PRESS, millis());
  }
}

void on_long_press() {
  if (s_active && s_step == Step::AWAIT_LONG_PRESS) {
    enter(Step::CELEBRATE, millis());
  }
}

void mark_done() {
  config::set_uint(KEY, 1);
  s_active = false;
  s_step   = Step::DONE;
  ::robimon::ui::screen_mgr::set_caption("", 0);
}

void replay() {
  config::set_uint(KEY, 0);
  s_active = true;
  enter(Step::WELCOME, millis());
}

}  // namespace robimon::services::tutorial
