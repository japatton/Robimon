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

// Quiet window after a swipe fires. The CST92xx touch controller occasionally
// reports n=0 mid-drag (single-frame glitch), which makes the gesture detector
// classify the partial drag as a swipe and the *remainder* of the drag as a
// fresh touch — that fresh touch then lands on the new screen as a tap (and
// pops open the radial menu). Swallowing all input for ~250 ms after a
// swipe means the actual continuation has time to lift cleanly first.
constexpr uint32_t POST_SWIPE_QUIET_MS = 250;
uint32_t s_quiet_until_ms      = 0;

Point    s_last_event_pt       = {0, 0};

}  // namespace

void begin() {
  s_touch_active = false;
  s_quiet_until_ms = 0;
}

Event update(int n_touches, Point pt) {
  const bool now_down = (n_touches > 0);
  const uint32_t now = millis();

  // Quiet window after a swipe (see POST_SWIPE_QUIET_MS comment). Drop the
  // touch state so the next post-quiet touch begins cleanly.
  if (now < s_quiet_until_ms) {
    s_touch_active = false;
    return Event::NONE;
  }

  // --- Touch begin ----------------------------------------------------------
  if (now_down && !s_touch_active) {
    s_touch_active = true;
    s_touch_start_ms = now;
    s_touch_start_pt = pt;
    s_touch_last_pt  = pt;
    return Event::NONE;
  }

  // --- Touch held -----------------------------------------------------------
  // Just track the latest position; we classify on release. This means a
  // pause-then-drag is reliably a swipe instead of getting preempted by a
  // mid-hold long-press event.
  if (now_down && s_touch_active) {
    s_touch_last_pt = pt;
    return Event::NONE;
  }

  // --- Touch released -------------------------------------------------------
  if (!now_down && s_touch_active) {
    s_touch_active = false;

    const uint32_t dur = now - s_touch_start_ms;
    const int dx = s_touch_last_pt.x - s_touch_start_pt.x;
    const int dy = s_touch_last_pt.y - s_touch_start_pt.y;
    const int abs_dx = abs(dx);
    const int abs_dy = abs(dy);

    // Horizontal swipe wins over everything — if the finger moved enough,
    // it's a swipe regardless of how long the touch lasted. Open a quiet
    // window so the touch controller's mid-drag glitches don't get
    // re-interpreted as a tap on the destination screen.
    if (abs_dx > SWIPE_MIN_PX && abs_dx > abs_dy) {
      s_last_event_pt = s_touch_start_pt;
      s_quiet_until_ms = now + POST_SWIPE_QUIET_MS;
      return (dx > 0) ? Event::SWIPE_RIGHT : Event::SWIPE_LEFT;
    }

    // Long-press: held past the threshold without much drift.
    if (dur >= LONG_PRESS_MS && abs_dx <= LP_MAX_MOTION_PX && abs_dy <= LP_MAX_MOTION_PX) {
      s_last_event_pt = s_touch_start_pt;
      return Event::LONG_PRESS;
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
