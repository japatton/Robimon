// Face renderer — parametric vector face drawn directly to the panel.
// Owns the FaceParams state, tween between expressions, and idle behaviors
// (blink / breathing / saccade).
//
// Renders via Arduino_GFX into a PSRAM-backed Arduino_Canvas (the display
// HAL flushes the canvas band to the QSPI panel in one shot). LVGL was
// considered early on but never used; it was dropped to save flash + RAM.

#pragma once

#include <stdint.h>

namespace robimon::face {

enum class Expression : uint8_t {
  NEUTRAL,
  HAPPY,
  SAD,
  SLEEPY,
  SURPRISED,
  ANGRY,
  THINKING,
  EXCITED,
  CONFUSED,
  LISTENING,
  SPEAKING,
  // LOVE — special-cased heart eyes; deferred until we have an icon path
};

bool begin();

// Set the target expression. The renderer tweens current → target over
// `tween_ms` (snappy for surprised/excited, slow for sleepy/sad).
void set_expression(Expression e, uint16_t tween_ms = 250);

// Call from the main loop. Advances tween + idle behaviors and redraws if
// anything changed. Cheap if nothing changed.
void update();

// Toggles a simple demo loop that cycles through expressions every few
// seconds. Useful for visual verification before the touch UI is in.
void enable_demo_cycle(bool on);

// Current expression name as a c-string (for logs).
const char* current_name();

// ---- Touch interaction (Stage D) -------------------------------------------
// Coordinates are panel-space (0..LCD_WIDTH × 0..LCD_HEIGHT).

// Forward a tap to the face. If a menu is open, hit-tests menu items; if
// idle, opens the radial expression menu at the tap location.
void on_tap(int panel_x, int panel_y);

// True if the radial menu is currently displayed. Useful for routing other
// gestures (swipes are ignored while the menu is open, etc.).
bool menu_is_open();

// Force-dismiss the radial menu if it's open. Used by the double-tap
// handler in main.cpp to roll back the menu the first TAP just opened.
void dismiss_menu();

// Show a short "we saw your gesture" text flash. Used as a visual confirm
// for long-press / swipe until the screen-manager and settings UI land.
void flash_text(const char* text);

// Persistent label overlay rendered below the eyes/mouth. Used by the alarm
// firing flow (e.g., set_label("test alarm")). Pass nullptr to clear.
void set_label(const char* text);
void clear_label();

}  // namespace robimon::face
