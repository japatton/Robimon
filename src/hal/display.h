// Display HAL — wraps Arduino_GFX (CO5300 over QSPI) and an Arduino_Canvas
// back-buffer in PSRAM. All drawing goes through the canvas; flush() pushes
// the buffer to the panel in one QSPI burst, gated on the CO5300's TE pin.
//
// Direct-to-panel writes are not supported: writing primitives during the
// panel's scanout produces visible tearing on AMOLED. The back-buffer is
// 466×466 RGB565 in PSRAM (~434 KB) — fits easily in our 8 MB OPI PSRAM.
//
// CO5300 hardware quirks worth knowing:
//   - 6-pixel column window offset baked into address registers.
//     Arduino_CO5300 takes this as col_offset1; we pass 6.
//   - QSPI runs at the panel driver's default speed; ~80 MHz on this MCU.
//   - TE pin (GPIO 13) pulses high once per refresh; flush() blocks on the
//     rising edge (with a bounded timeout) before pushing.

#pragma once

#include <stdint.h>

class Arduino_GFX;

namespace robimon::hal::display {

bool begin();

// Set panel brightness (0..255). Implemented via CO5300 register write.
// This is the user-set ceiling — apply_idle_brightness() may render at a
// fraction of this (dim) or skip drawing (blank) without overwriting it.
void set_brightness(uint8_t brightness_0_255);

// Returns the user-set brightness (most recent value passed to set_brightness).
uint8_t user_brightness();

// Apply auto-dim / blank-out based on how long since the last user activity.
// idle_ms < AUTO_DIM_MS         → full user brightness
// AUTO_DIM_MS ≤ idle < BLANK_MS → dim (10 % of user)
// idle_ms ≥ AUTO_BLANK_MS       → off
// Cheap to call every loop iteration; only writes the panel register on
// transitions, so this is not a hot path.
void apply_idle_brightness(uint32_t idle_ms);

constexpr uint32_t AUTO_DIM_MS    = 60UL  * 1000UL;    //  1 min → dim
constexpr uint32_t AUTO_BLANK_MS  = 300UL * 1000UL;    //  5 min → off

// Returns the back-buffer GFX. Draw your frame here, then call flush().
// Methods inherit from Arduino_GFX: fillRect, fillCircle, fillEllipse,
// fillTriangle, fillArc, drawLine, setCursor/print, etc.
Arduino_GFX* gfx();

// Wait for the next TE rising edge (≈ start of vsync porch). Bounded so a
// stuck TE never deadlocks the caller.
void wait_for_te();

// Push the back-buffer to the panel in one QSPI burst. Cheap to call back
// to back; you decide the frame cadence.
void flush();

// Push only a horizontal band of the canvas (full canvas width, [y, y+h)
// in canvas-local coords). Useful for press-state animations on a single
// button where the rest of the screen is unchanged — saves the bulk of
// the QSPI bandwidth a full flush would burn. Out-of-range (x, y, h)
// values are clamped.
void flush_band(int y, int h);

int width();   // canvas width  (panel-equivalent x)
int height();  // canvas height (NOT panel height — see canvas_panel_y_offset())

// The canvas covers a band of the panel; its local (0, 0) maps to panel
// (0, canvas_panel_y_offset()). Use canvas-local coords when drawing.
int canvas_panel_y_offset();

}  // namespace robimon::hal::display
