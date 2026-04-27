// First-boot gesture tutorial. Walks the kid through tap-mouth, swipe, and
// long-press once, then disables itself permanently (NVS flag tutorial_done).
//
// Observational, not interceptive — the user's gestures still flow normally
// through the screen manager and voice/settings code paths. Tutorial just
// watches for the right event and advances its caption.

#pragma once

#include <stdint.h>

namespace robimon::services::tutorial {

bool begin();          // returns true if tutorial is now active
bool is_active();
void update(uint32_t now_ms);

// Event hooks called from main.cpp's gesture dispatch.
void on_mouth_tap();
void on_swipe();
void on_long_press();

// Forces the tutorial to "done" — used by factory-reset / re-run-setup
// paths so the user doesn't get the tutorial again unless they want to.
void mark_done();

// Re-enables the tutorial — dropped onto the device by the about screen
// or serial console for a kid who wants to see it again.
void replay();

}  // namespace robimon::services::tutorial
