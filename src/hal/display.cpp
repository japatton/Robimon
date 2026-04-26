#include "display.h"
#include "board.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace robimon::hal::display {

namespace {
constexpr const char* TAG = "lcd";

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX*     s_gfx = nullptr;
bool             s_te_attached = false;

// CO5300 ctor: bus, rst, rotation, w, h, col_offset1, row_offset1, col_offset2, row_offset2.
// Last three are rotation-dependent offsets the driver applies to address-window writes.
// Stock Waveshare example passes (6, 0, 0, 0); rotation 0 only uses col_offset1.
}  // namespace

bool begin() {
  using namespace robimon::board;

  s_bus = new Arduino_ESP32QSPI(
      LCD_CS, LCD_QSPI_SCLK,
      LCD_QSPI_D0, LCD_QSPI_D1, LCD_QSPI_D2, LCD_QSPI_D3);

  s_gfx = new Arduino_CO5300(
      s_bus, LCD_RESET, /*rotation=*/0, LCD_WIDTH, LCD_HEIGHT,
      LCD_COL_OFFSET, 0, 0, 0);

  if (!s_gfx->begin()) {
    LOG_E(TAG, "Arduino_CO5300::begin() failed");
    return false;
  }

  s_gfx->fillScreen(0x0000);
  s_gfx->setBrightness(180);

  // Wire the TE pin as a plain input for now. We poll it from wait_for_te()
  // when the caller needs vsync; later we can switch to an attachInterrupt
  // and a notify if we want to free CPU during the wait.
  pinMode(LCD_TE, INPUT);
  s_te_attached = true;

  LOG_I(TAG, "CO5300 %dx%d up", LCD_WIDTH, LCD_HEIGHT);
  return true;
}

void set_brightness(uint8_t b) {
  if (s_gfx) s_gfx->setBrightness(b);
}

void flush(int x1, int y1, int x2, int y2, const uint16_t* px) {
  if (!s_gfx) return;
  const int w = x2 - x1 + 1;
  const int h = y2 - y1 + 1;
  // LV_COLOR_16_SWAP=1 means LVGL hands us already-byte-swapped RGB565.
  s_gfx->draw16bitBeRGBBitmap(x1, y1, const_cast<uint16_t*>(px), w, h);
}

void wait_for_te() {
  if (!s_te_attached) return;
  // CO5300 TE pulses high once per frame. Spin briefly, with a microsecond
  // sleep so we don't pin the core. Bounded so a stuck TE never deadlocks us.
  const uint32_t deadline_us = micros() + 20000;  // 20 ms ≈ 50 Hz floor
  while (digitalRead(robimon::board::LCD_TE) == LOW && micros() < deadline_us) { delayMicroseconds(50); }
  while (digitalRead(robimon::board::LCD_TE) == HIGH && micros() < deadline_us) { delayMicroseconds(50); }
}

Arduino_GFX* gfx() { return s_gfx; }
int width()  { return s_gfx ? s_gfx->width()  : 0; }
int height() { return s_gfx ? s_gfx->height() : 0; }

}  // namespace robimon::hal::display
