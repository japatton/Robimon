#include "gestures.h"

#include <Arduino.h>
#include <stdlib.h>

namespace robimon::ui::gestures {

namespace {

// Tunable thresholds. Generous on motion since the round AMOLED's edges
// can read jittery if a fingertip is partly off-glass and a held finger
// naturally drifts a few pixels.
constexpr uint32_t TAP_MAX_MS         = 350;
constexpr uint32_t LONG_PRESS_MS      = 1200;
constexpr int      SWIPE_MIN_PX       = 50;
constexpr int      TAP_MAX_MOTION_PX  = 40;
constexpr int      LP_MAX_MOTION_PX   = 50;   // long-press is more forgiving than tap

bool     s_touch_active        = false;
uint32_t s_touch_start_ms      = 0;
Point    s_touch_start_pt      = {0, 0};
Point    s_touch_last_pt       = {0, 0};
bool     s_long_press_emitted  = false;

Point    s_last_event_pt       = {0, 0};

}  // namespace

void begin() {
  s_touch_active = false;
  s_long_press_emitted = false;
}

Event update(int n_touches, Point pt) {
  const bool now_down = (n_touches > 0);
  const uint32_t now = millis();

  // --- Touch begin ----------------------------------------------------------
  if (now_down && !s_touch_active) {
    s_touch_active = true;
    s_touch_start_ms = now;
    s_touch_start_pt = pt;
    s_touch_last_pt  = pt;
    s_long_press_emitted = false;
    return Event::NONE;
  }

  // --- Touch held -----------------------------------------------------------
  if (now_down && s_touch_active) {
    s_touch_last_pt = pt;

    // Long-press fires while the finger is still down so the UI can respond
    // immediately (e.g., open the PIN pad). We only fire it once per touch.
    if (!s_long_press_emitted && (now - s_touch_start_ms) > LONG_PRESS_MS) {
      const int dx = pt.x - s_touch_start_pt.x;
      const int dy = pt.y - s_touch_start_pt.y;
      if (abs(dx) <= LP_MAX_MOTION_PX && abs(dy) <= LP_MAX_MOTION_PX) {
        s_long_press_emitted = true;
        s_last_event_pt = pt;
        return Event::LONG_PRESS;
      }
    }
    return Event::NONE;
  }

  // --- Touch released -------------------------------------------------------
  if (!now_down && s_touch_active) {
    s_touch_active = false;
    if (s_long_press_emitted) return Event::NONE;

    const uint32_t dur = now - s_touch_start_ms;
    const int dx = s_touch_last_pt.x - s_touch_start_pt.x;
    const int dy = s_touch_last_pt.y - s_touch_start_pt.y;
    const int abs_dx = abs(dx);
    const int abs_dy = abs(dy);

    // Horizontal swipe takes priority over vertical for our left/right nav.
    if (abs_dx > SWIPE_MIN_PX && abs_dx > abs_dy) {
      s_last_event_pt = s_touch_start_pt;
      return (dx > 0) ? Event::SWIPE_RIGHT : Event::SWIPE_LEFT;
    }

    // TAP: brief contact with very little drift.
    if (dur < TAP_MAX_MS && abs_dx <= TAP_MAX_MOTION_PX && abs_dy <= TAP_MAX_MOTION_PX) {
      s_last_event_pt = s_touch_start_pt;
      return Event::TAP;
    }
  }

  return Event::NONE;
}

Point last_event_point() { return s_last_event_pt; }

}  // namespace robimon::ui::gestures
