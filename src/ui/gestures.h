// Gesture detector — consumes raw touch points and emits high-level gestures.
//
// Stage D-1 wires up: TAP (brief touch + release with little motion), LONG_PRESS
// (held >1.5 s without moving), SWIPE_LEFT, SWIPE_RIGHT. DOUBLE_TAP is reserved
// for the voice mode in stage F and not emitted yet.
//
// Usage: call update(n_touches, first_point) from the main loop on every
// iteration with the current touch state. Returns the latest event (NONE if
// nothing fired this tick). Read last_event_point() right after to find out
// where the gesture happened.

#pragma once

#include <stdint.h>

namespace robimon::ui::gestures {

enum class Event : uint8_t {
  NONE,
  TAP,
  LONG_PRESS,
  SWIPE_LEFT,
  SWIPE_RIGHT,
};

struct Point { int16_t x, y; };

void begin();

// n_touches: number of active touch points reported by the touch HAL this tick.
// first_point: the location of touch #0 (ignored if n_touches == 0).
Event update(int n_touches, Point first_point);

// Where the most recently fired gesture occurred. For TAP/LONG_PRESS, the
// touch-down location; for swipes, the start of the swipe.
Point last_event_point();

}  // namespace robimon::ui::gestures
