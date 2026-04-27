#include "display.h"
#include "board.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

namespace robimon::hal::display {

namespace {
constexpr const char* TAG = "lcd";

// Size of the back-buffer covering the face area. The canvas's local (0,0)
// maps to panel (0, CANVAS_OUTPUT_Y). When you need a panel y, add this; when
// drawing into the canvas, use canvas-local coords.
// 320 tall accommodates eyes + mouth + cheek blushes; flush ≈ 30 ms at 80 MHz.
constexpr int CANVAS_H        = 320;
constexpr int CANVAS_OUTPUT_Y = (466 - CANVAS_H) / 2;   // 73

Arduino_DataBus* s_bus      = nullptr;
Arduino_CO5300*  s_panel    = nullptr;
Arduino_Canvas*  s_canvas   = nullptr;

// Idle-aware brightness state. user_b is the most-recent user-set level
// (the ceiling); current_b is what the panel is actually rendering at.
// We only push to the panel when current_b changes — keeps apply_idle_*
// cheap to call every loop iteration.
uint8_t s_user_b    = 180;
uint8_t s_current_b = 180;
}  // namespace

bool begin() {
  using namespace robimon::board;

  s_bus = new Arduino_ESP32QSPI(
      LCD_CS, LCD_QSPI_SCLK,
      LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);

  s_panel = new Arduino_CO5300(
      s_bus, LCD_RESET, /*rotation=*/0, LCD_WIDTH, LCD_HEIGHT,
      LCD_COL_OFFSET, 0, 0, 0);

  // Back-buffer covering just the face band, not the full panel. The flush
  // is the bottleneck (≈ 1 byte / 10 ns over QSPI) so a smaller canvas =
  // higher achievable FPS. 466×280 ≈ 261 KB; flush ≈ 26 ms; theoretical max
  // ~38 FPS. Full-screen would be 434 KB / 43 ms / 23 FPS.
  // Top of canvas at panel y=93, so face center (panel y=233) lands at
  // canvas-local y = 140. Bottom at panel y=373.
  s_canvas = new Arduino_Canvas(LCD_WIDTH, /*h=*/CANVAS_H, s_panel,
                                /*output_x=*/0, /*output_y=*/CANVAS_OUTPUT_Y);

  // Canvas::begin() internally calls panel->begin(). Calling panel->begin()
  // ourselves first would assert on the QSPI bus being already initialized.
  // 80 MHz QSPI — CO5300 supports it; default ESP32QSPI_FREQUENCY is 40 MHz
  // which limits us to ~12 FPS on a 466×280 buffer. Drop to 60 MHz if we
  // ever see flush corruption (haven't observed any so far).
  constexpr int32_t QSPI_HZ = 80 * 1000 * 1000;
  if (!s_canvas->begin(QSPI_HZ)) {
    LOG_E(TAG, "Arduino_Canvas::begin() failed (likely PSRAM allocation)");
    return false;
  }

  // Clear the full panel once (direct write — slow, but only happens at boot).
  // Anything outside the canvas band stays black for the rest of the session
  // since we never push to it again.
  s_panel->fillScreen(0x0000);

  s_canvas->fillScreen(0x0000);
  s_canvas->flush();
  s_user_b    = 180;
  s_current_b = 180;
  s_panel->setBrightness(s_user_b);

  // TE pin as plain input; flush() polls it before pushing.
  pinMode(LCD_TE, INPUT);

  LOG_I(TAG, "CO5300 %dx%d up; canvas in PSRAM, free PSRAM %lu",
        LCD_WIDTH, LCD_HEIGHT,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return true;
}

void set_brightness(uint8_t b) {
  s_user_b = b;
  s_current_b = b;
  if (s_panel) s_panel->setBrightness(b);
}

uint8_t user_brightness() { return s_user_b; }

void apply_idle_brightness(uint32_t idle_ms) {
  // Compute the desired level. Dim is 10 % of user (rounded up so a
  // user_b=16 doesn't go fully black on dim).
  uint8_t want;
  if      (idle_ms >= AUTO_BLANK_MS) want = 0;
  else if (idle_ms >= AUTO_DIM_MS)   want = (uint8_t)((s_user_b + 9) / 10);
  else                                want = s_user_b;

  if (want != s_current_b) {
    s_current_b = want;
    if (s_panel) s_panel->setBrightness(want);
  }
}

Arduino_GFX* gfx() { return s_canvas; }
int width()  { return s_canvas ? s_canvas->width()  : 0; }
int height() { return s_canvas ? s_canvas->height() : 0; }
int canvas_panel_y_offset() { return CANVAS_OUTPUT_Y; }

void wait_for_te() {
  // CO5300 TE pulses high once per refresh. Spin briefly with microsecond
  // sleeps so we don't pin the core. Bounded so a stuck TE never deadlocks us.
  const uint32_t deadline_us = micros() + 20000;  // 20 ms ≈ 50 Hz floor
  while (digitalRead(robimon::board::LCD_TE) == HIGH && micros() < deadline_us) { delayMicroseconds(50); }
  while (digitalRead(robimon::board::LCD_TE) == LOW  && micros() < deadline_us) { delayMicroseconds(50); }
}

void flush() {
  if (s_canvas) s_canvas->flush();
}

void clear_screen_area(uint16_t color) {
  if (!s_canvas) return;
  const int top = OVERLAY_TOP_RESERVED;
  const int h   = CANVAS_H - OVERLAY_TOP_RESERVED - OVERLAY_BOTTOM_RESERVED;
  if (h > 0) s_canvas->fillRect(0, top, s_canvas->width(), h, color);
}

void flush_band(int y, int h) {
  if (!s_canvas || !s_panel || h <= 0) return;
  if (y < 0)              { h += y; y = 0; }
  if (y >= CANVAS_H)      return;
  if (y + h > CANVAS_H)   h = CANVAS_H - y;
  if (h <= 0)             return;
  // Full-width slice of the framebuffer — contiguous in memory, so we can
  // hand it to the panel as a single bitmap write.
  uint16_t* fb = (uint16_t*)s_canvas->getFramebuffer();
  if (!fb) return;
  s_panel->draw16bitRGBBitmap(0, CANVAS_OUTPUT_Y + y,
                               fb + (size_t)y * (size_t)s_canvas->width(),
                               s_canvas->width(), h);
}

}  // namespace robimon::hal::display
