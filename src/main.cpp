// Robimon — entry point. Brings up the HAL stack and runs a placeholder
// idle loop with stats reporting. Real screens, animation, and services
// land in later stages.
//
// Init order matters:
//   1. Serial (USB-CDC)
//   2. Wire (shared I2C bus — must be up before any chip wrappers)
//   3. I/O expander (TCA9554 — owns SYS_OUT/PWR-on-gate; bring up early so
//      a buggy later module can't accidentally power us down)
//   4. PMIC (AXP2101)
//   5. RTC (later)
//   6. Display (CO5300)
//   7. Touch (CST9217)
//   8. IMU (QMI8658)
//   9. Audio (ES8311 codec + I2S + PA)
//
// Set ROBIMON_BOOT_TEST_TONE=1 in build_flags to play a 120 ms beep at end
// of setup as a quick audio bring-up confirmation.

#include <Arduino.h>
#include <Wire.h>

#include "hal/board.h"
#include "hal/io_expander.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "hal/imu.h"
#include "hal/audio.h"
#include "app/log.h"
#include "app/stats.h"

#include <Arduino_GFX_Library.h>

namespace {
constexpr const char* TAG = "main";

void draw_boot_face(Arduino_GFX* g) {
  using namespace robimon::board;
  constexpr int CX = LCD_WIDTH / 2;
  constexpr int CY = LCD_HEIGHT / 2;

  g->fillScreen(0x0000);

  const int eye_offset = 70;
  const int eye_radius = 48;
  const int pupil_radius = 18;

  g->fillCircle(CX - eye_offset, CY - 20, eye_radius, 0xFFFF);
  g->fillCircle(CX + eye_offset, CY - 20, eye_radius, 0xFFFF);
  g->fillCircle(CX - eye_offset, CY - 20, pupil_radius, 0x18FF);
  g->fillCircle(CX + eye_offset, CY - 20, pupil_radius, 0x18FF);
  g->fillCircle(CX - eye_offset + 6, CY - 26, 5, 0xFFFF);
  g->fillCircle(CX + eye_offset + 6, CY - 26, 5, 0xFFFF);
  g->fillRoundRect(CX - 32, CY + 70, 64, 10, 5, 0xFFFF);

  g->setTextColor(0xFFFF);
  g->setCursor(CX - 56, CY + 110);
  g->setTextSize(2, 2, 0);
  g->print("Robimon");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  LOG_I(TAG, "Robimon %s booting", ROBIMON_VERSION);

  Wire.begin(robimon::board::I2C_SDA, robimon::board::I2C_SCL, robimon::board::I2C_FREQ_HZ);
  LOG_I(TAG, "I2C up: SDA=%d SCL=%d @ %lu Hz",
        robimon::board::I2C_SDA, robimon::board::I2C_SCL, (unsigned long)robimon::board::I2C_FREQ_HZ);

  // Order matters here — see the file header comment.
  if (!robimon::hal::ioexp::begin())   LOG_W(TAG, "ioexp not ready");
  if (!robimon::hal::power::begin())   LOG_W(TAG, "PMIC not ready");

  if (!robimon::hal::display::begin()) {
    LOG_E(TAG, "display init failed; halting");
    while (true) { delay(1000); }
  }

  if (!robimon::hal::touch::begin())   LOG_W(TAG, "touch not ready");
  if (!robimon::hal::imu::begin())     LOG_W(TAG, "IMU not ready");
  if (!robimon::hal::audio::begin())   LOG_W(TAG, "audio not ready");

  draw_boot_face(robimon::hal::display::gfx());
  robimon::stats::begin();

#if defined(ROBIMON_BOOT_TEST_TONE) && (ROBIMON_BOOT_TEST_TONE == 1)
  if (robimon::hal::audio::ok()) {
    LOG_I(TAG, "playing boot test tone");
    robimon::hal::audio::play_test_tone();
  }
#endif

  LOG_I(TAG, "setup done");
}

void loop() {
  robimon::hal::touch::Point pts[1];
  const int n = robimon::hal::touch::read(pts, 1);
  if (n > 0) {
    auto* g = robimon::hal::display::gfx();
    g->fillCircle(pts[0].x, pts[0].y, 6, 0xF800);
    LOG_D(TAG, "touch %d,%d", pts[0].x, pts[0].y);
  }

  robimon::stats::note_frame();
  robimon::stats::tick();
  delay(5);
}
