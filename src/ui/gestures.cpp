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
constexpr int      LP_MAX_MOTION_PX   = 50;

// Release debounce: when the touch controller reports n=0, wait this long
// before confirming the release. The CST9217 occasionally drops a frame
// mid-press (touch flutter); without this debounce, a single physical
// swipe gets fragmented into multiple short non-events that each fail
// the SWIPE_MIN_PX threshold. A continuation within the debounce window
// is treated as the same touch — start_pt is preserved.
constexpr uint32_t RELEASE_DEBOUNCE_MS = 40;

// Quiet windows after a gesture fires: ignore all touch input for a brief
// period so the actual finger lift doesn't get re-interpreted (a tap on
// the destination screen continuing a swipe, or a glitch-fragmented tap
// firing 2-3 PIN digits per press).
constexpr uint32_t POST_TAP_QUIET_MS   = 200;
constexpr uint32_t POST_LP_QUIET_MS    = 200;
constexpr uint32_t POST_SWIPE_QUIET_MS = 250;

enum class State : uint8_t { IDLE, ACTIVE, RELEASING };

State    s_state               = State::IDLE;
uint32_t s_touch_start_ms      = 0;
uint32_t s_release_check_ms    = 0;
Point    s_touch_start_pt      = {0, 0};
Point    s_touch_last_pt       = {0, 0};
uint32_t s_quiet_until_ms      = 0;
Point    s_last_event_pt       = {0, 0};

}  // namespace

void begin() {
  s_state = State::IDLE;
  s_quiet_until_ms = 0;
}

Event update(int n_touches, Point pt) {
  const bool now_down = (n_touches > 0);
  const uint32_t now = millis();

  // Quiet window after any gesture — drop state so we re-enter cleanly.
  if (now < s_quiet_until_ms) {
    s_state = State::IDLE;
    return Event::NONE;
  }

  switch (s_state) {
    case State::IDLE:
      if (now_down) {
        s_state = State::ACTIVE;
        s_touch_start_ms = now;
        s_touch_start_pt = pt;
        s_touch_last_pt  = pt;
      }
      return Event::NONE;

    case State::ACTIVE:
      if (now_down) {
        s_touch_last_pt = pt;
        return Event::NONE;
      }
      // First absent frame — defer the classification by RELEASE_DEBOUNCE_MS
      // in case this is just controller flutter and the touch is about to
      // come back at the same position.
      s_state = State::RELEASING;
      s_release_check_ms = now + RELEASE_DEBOUNCE_MS;
      return Event::NONE;

    case State::RELEASING:
      if (now_down) {
        // Touch came back within the debounce window — flutter, not a real
        // release. Keep the existing start_pt so the swipe accumulates
        // motion across the glitch.
        s_state = State::ACTIVE;
        s_touch_last_pt = pt;
        return Event::NONE;
      }
      if (now < s_release_check_ms) {
        return Event::NONE;   // still waiting for debounce to expire
      }
      // Confirmed release. Classify based on accumulated motion + duration.
      {
        s_state = State::IDLE;
        const uint32_t dur = now - s_touch_start_ms;
        const int dx = s_touch_last_pt.x - s_touch_start_pt.x;
        const int dy = s_touch_last_pt.y - s_touch_start_pt.y;
        const int abs_dx = abs(dx);
        const int abs_dy = abs(dy);

        // Horizontal swipe wins over everything if the finger moved enough.
        if (abs_dx > SWIPE_MIN_PX && abs_dx > abs_dy) {
          s_last_event_pt = s_touch_start_pt;
          s_quiet_until_ms = now + POST_SWIPE_QUIET_MS;
          return (dx > 0) ? Event::SWIPE_RIGHT : Event::SWIPE_LEFT;
        }
        // Long-press: held past threshold without much drift.
        if (dur >= LONG_PRESS_MS && abs_dx <= LP_MAX_MOTION_PX && abs_dy <= LP_MAX_MOTION_PX) {
          s_last_event_pt = s_touch_start_pt;
          s_quiet_until_ms = now + POST_LP_QUIET_MS;
          return Event::LONG_PRESS;
        }
        // TAP: brief contact with very little drift.
        if (dur < TAP_MAX_MS && abs_dx <= TAP_MAX_MOTION_PX && abs_dy <= TAP_MAX_MOTION_PX) {
          s_last_event_pt = s_touch_start_pt;
          s_quiet_until_ms = now + POST_TAP_QUIET_MS;
          return Event::TAP;
        }
        return Event::NONE;
      }
  }
  return Event::NONE;
}

Point last_event_point() { return s_last_event_pt; }

}  // namespace robimon::ui::gestures
