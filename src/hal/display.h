// Display HAL — wraps Arduino_GFX (CO5300 over QSPI) with the small
// surface area the rest of the firmware needs. The face renderer and
// LVGL bind to this; nobody else should #include Arduino_GFX directly.
//
// CO5300 hardware quirks worth knowing:
//   - 6-pixel column window offset baked into address registers.
//     The Arduino_CO5300 constructor takes this as col_offset1; we pass 6.
//   - QSPI runs at ~80 MHz with this driver; double-buffered DMA flushes
//     happen out of PSRAM (LVGL allocates the buffers, we just push them).
//   - TE pin (GPIO 13) is wired and useful for tear-free transitions.
//     Phase 1: ignored. Phase 2: gate flush on TE rising edge.

#pragma once

#include <stdint.h>

class Arduino_DataBus;
class Arduino_GFX;

namespace robimon::hal::display {

bool begin();

// Set panel brightness (0..255). Implemented via CO5300 register write.
void set_brightness(uint8_t brightness_0_255);

// Push a raw RGB565 region to the panel. Used by LVGL flush callback.
// Coordinates are inclusive; data is row-major, byte-swapped (matches LV_COLOR_16_SWAP=1).
void flush(int x1, int y1, int x2, int y2, const uint16_t* px);

// Block until the next TE pulse. No-op until we wire it up; see header comment.
void wait_for_te();

// Escape hatch for early bring-up when we just want to draw shapes directly.
// Avoid in production code — go through LVGL.
Arduino_GFX* gfx();

int width();
int height();

}  // namespace robimon::hal::display
