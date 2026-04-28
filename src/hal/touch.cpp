#include "touch.h"
#include "board.h"
#include "../app/log.h"

#include <Arduino.h>
#include <Wire.h>
#include <TouchDrv.hpp>

namespace robimon::hal::touch {

namespace {
constexpr const char* TAG = "touch";

TouchDrvCST92xx s_drv;
bool            s_ready = false;
}  // namespace

bool begin() {
  using namespace robimon::board;

  s_drv.setPins(TP_RESET, TP_INT);
  // Pass -1/-1 for sda/scl: Wire is already initialized in main.
  // Re-passing the pins triggers a noisy "bus already initialized" warning.
  if (!s_drv.begin(Wire, TP_I2C_ADDR, -1, -1)) {
    LOG_E(TAG, "CST9217 not found at 0x%02X", TP_I2C_ADDR);
    return false;
  }

  s_drv.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  // The reference Waveshare ES8311 example sets both mirrors true;
  // this aligns the touch coordinate system with the display orientation
  // we use (rotation=0, no display flip). Empirical, not from datasheet.
  s_drv.setMirrorXY(true, true);

  // INT line as plain INPUT (the CST92xx driver holds the pin floating
  // most of the time and drives it LOW for the duration of an active
  // touch). The main loop's read() now early-returns when INT is HIGH
  // — no I2C traffic until the kid actually touches the screen.
  pinMode(TP_INT, INPUT);

  s_ready = true;
  LOG_I(TAG, "CST9217 up @ 0x%02X (INT-driven)", TP_I2C_ADDR);
  return true;
}

int read(Point* out_points, int max_points) {
  if (!s_ready || !out_points || max_points <= 0) return 0;

  // Fast path: nothing is touching the screen → INT is high → skip the
  // I2C burst entirely. With ~500 Hz loop rate and a typical touch
  // duration of < 5 % of wall time, this drops touch I2C traffic by
  // > 95 %, freeing up bus time for audio + IMU and cutting ~1-2 mA
  // average current. The 1 ms I2C cost was the dominant per-iteration
  // hot-path expense in the perf review.
  if (digitalRead(robimon::board::TP_INT) == HIGH) return 0;

  const TouchPoints& tp = s_drv.getTouchPoints();
  const int n = (int)tp.getPointCount();
  const int report_n = n < max_points ? n : max_points;
  for (int i = 0; i < report_n; ++i) {
    const TouchPoint& pt = tp.getPoint((uint8_t)i);
    out_points[i].x = (int16_t)pt.x;
    out_points[i].y = (int16_t)pt.y;
  }
  return report_n;
}

}  // namespace robimon::hal::touch
