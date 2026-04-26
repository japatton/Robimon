// Robimon — entry point. Wires up the HAL, drops a placeholder face
// (a centered greeting + a touch-tracking dot) onto the screen, and
// reports stats over USB-CDC every 5 s.
//
// What's NOT here yet: face renderer, expression library, LVGL screens,
// audio, IMU, PMIC, expander, alarms, services. All come in later stages.

#include <Arduino.h>
#include <Wire.h>

#include "hal/board.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "app/log.h"
#include "app/stats.h"

#include <Arduino_GFX_Library.h>

namespace {
constexpr const char* TAG = "main";

void draw_boot_face(Arduino_GFX* g) {
  // Two big eyes + a small mouth, all parametric. Placeholder until the
  // real face renderer lands; useful as a "did the panel boot?" signal.
  using namespace robimon::board;
  constexpr int CX = LCD_WIDTH / 2;
  constexpr int CY = LCD_HEIGHT / 2;

  g->fillScreen(0x0000);

  const int eye_offset = 70;
  const int eye_radius = 48;
  const int pupil_radius = 18;

  // Eye whites
  g->fillCircle(CX - eye_offset, CY - 20, eye_radius, 0xFFFF);
  g->fillCircle(CX + eye_offset, CY - 20, eye_radius, 0xFFFF);
  // Pupils (RGB565 deep blue)
  g->fillCircle(CX - eye_offset, CY - 20, pupil_radius, 0x18FF);
  g->fillCircle(CX + eye_offset, CY - 20, pupil_radius, 0x18FF);
  // Pupil highlight
  g->fillCircle(CX - eye_offset + 6, CY - 26, 5, 0xFFFF);
  g->fillCircle(CX + eye_offset + 6, CY - 26, 5, 0xFFFF);

  // Mouth: small curved line approximated by a filled arc-ish rectangle.
  // The real face renderer will use a bezier; this is a placeholder.
  g->fillRoundRect(CX - 32, CY + 70, 64, 10, 5, 0xFFFF);

  // Title
  g->setTextColor(0xFFFF);
  g->setCursor(CX - 56, CY + 110);
  g->setTextSize(2, 2, 0);
  g->print("Robimon");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Native USB-CDC needs a moment after enumeration before logs are visible.
  delay(200);
  LOG_I(TAG, "Robimon %s booting", ROBIMON_VERSION);

  Wire.begin(robimon::board::I2C_SDA, robimon::board::I2C_SCL, robimon::board::I2C_FREQ_HZ);
  LOG_I(TAG, "I2C up: SDA=%d SCL=%d @ %lu Hz",
        robimon::board::I2C_SDA, robimon::board::I2C_SCL, (unsigned long)robimon::board::I2C_FREQ_HZ);

  if (!robimon::hal::display::begin()) {
    LOG_E(TAG, "display init failed; halting");
    while (true) { delay(1000); }
  }

  if (!robimon::hal::touch::begin()) {
    LOG_W(TAG, "touch init failed; continuing without input");
  }

  draw_boot_face(robimon::hal::display::gfx());
  robimon::stats::begin();
  LOG_I(TAG, "setup done");
}

void loop() {
  robimon::hal::touch::Point pts[1];
  const int n = robimon::hal::touch::read(pts, 1);
  if (n > 0) {
    auto* g = robimon::hal::display::gfx();
    // Tracking dot, drawn straight to the panel for now. Once LVGL is
    // owning the display, this debug indicator moves into a screen-local
    // overlay so we don't fight the partial-flush dirty tracking.
    g->fillCircle(pts[0].x, pts[0].y, 6, 0xF800);
    LOG_D(TAG, "touch %d,%d", pts[0].x, pts[0].y);
  }

  robimon::stats::note_frame();
  robimon::stats::tick();
  delay(5);
}
