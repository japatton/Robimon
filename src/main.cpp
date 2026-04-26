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

#include <Arduino.h>
#include <Wire.h>

#include "hal/board.h"
#include "hal/io_expander.h"
#include "hal/power.h"
#include "hal/display.h"
#include "hal/touch.h"
#include "hal/imu.h"
#include "hal/audio.h"
#include "face/face.h"
#include "ui/gestures.h"
#include "ui/screen_mgr.h"
#include "screens/face_screen.h"
#include "screens/alarms_screen.h"
#include "app/log.h"
#include "app/stats.h"

namespace {
constexpr const char* TAG = "main";
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

  robimon::face::begin();
  // Stage D: touch drives expressions. Demo cycle off by default; the user
  // can re-enable via face::enable_demo_cycle(true) for visual checks.
  robimon::face::enable_demo_cycle(false);

  robimon::ui::gestures::begin();

  // Stage E: screen manager. Face is the default screen (index 0); alarms
  // is the second. Settings is reached via long-press + PIN, NOT swipe.
  robimon::ui::screen_mgr::begin();
  robimon::ui::screen_mgr::add_screen(&robimon::screens::face_screen);
  robimon::ui::screen_mgr::add_screen(&robimon::screens::alarms_screen);

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
  // Read touch and feed it to the gesture detector. Events route through
  // the screen manager to the active screen (or to nav, in the case of
  // swipes).
  robimon::hal::touch::Point hw_pts[1];
  const int n = robimon::hal::touch::read(hw_pts, 1);
  robimon::ui::gestures::Point gp{ n > 0 ? hw_pts[0].x : (int16_t)0,
                                    n > 0 ? hw_pts[0].y : (int16_t)0 };
  const auto event = robimon::ui::gestures::update(n, gp);

  switch (event) {
    case robimon::ui::gestures::Event::TAP: {
      const auto pt = robimon::ui::gestures::last_event_point();
      robimon::ui::screen_mgr::on_tap(pt.x, pt.y);
      break;
    }
    case robimon::ui::gestures::Event::LONG_PRESS:
      // Long-press will open the PIN-locked settings menu in stage F.
      LOG_I(TAG, "long-press detected");
      robimon::face::flash_text("settings");
      break;
    case robimon::ui::gestures::Event::SWIPE_LEFT:
      // Swipes only navigate when the face's radial menu isn't open —
      // accidental swipes during expression-picking shouldn't change screen.
      if (!robimon::face::menu_is_open()) robimon::ui::screen_mgr::next();
      break;
    case robimon::ui::gestures::Event::SWIPE_RIGHT:
      if (!robimon::face::menu_is_open()) robimon::ui::screen_mgr::prev();
      break;
    default:
      break;
  }

  robimon::ui::screen_mgr::update(millis());
  robimon::stats::note_frame();
  robimon::stats::tick();
  delay(2);
}
