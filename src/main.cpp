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
#include "screens/pin_pad_screen.h"
#include "screens/settings_menu_screen.h"
#include "screens/wifi_setup_screen.h"
#include "screens/time_settings_screen.h"
#include "screens/display_settings_screen.h"
#include "screens/about_screen.h"
#include "screens/alarms_settings_screen.h"
#include "screens/alarm_edit_screen.h"
#include "screens/ha_settings_screen.h"
#include "screens/ha_entity_screen.h"
#include "services/config_store.h"
#include "services/wifi_mgr.h"
#include "services/time_svc.h"
#include "services/alarm_mgr.h"
#include "services/ha_client.h"
#include "services/serial_console.h"
#include "app/log.h"
#include "app/stats.h"

namespace { struct AlarmFireWatcher { int last_idx = -2; } s_alarm_watcher; }

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
  if (!robimon::services::config::begin()) LOG_W(TAG, "config store not ready");

  robimon::services::wifi_mgr::begin();
  // Best-effort auto-connect using whatever creds are saved in NVS. Silent
  // no-op on a fresh device.
  robimon::services::wifi_mgr::auto_connect();

  // Time service: applies stored TZ immediately; SNTP starts after WiFi connects.
  robimon::services::time_svc::begin();

  // Alarms: load saved alarms from NVS (firing happens once SNTP is synced).
  robimon::services::alarm_mgr::begin();

  // HA client: starts in DISCONNECTED state and reconnects with backoff
  // once WiFi comes up and creds are available.
  robimon::services::ha_client::begin();
  robimon::services::ha_client::auto_connect();

  // Serial console for pasting long values (HA tokens, URLs, prompts) from
  // the host terminal — much faster than the on-screen keyboard.
  robimon::services::serial_console::begin();

  // Apply stored brightness preference (default level 5 = 176/255).
  {
    const uint8_t LVLS[] = { 16, 48, 80, 112, 144, 176, 208, 240 };
    const uint32_t idx = robimon::services::config::get_uint("disp_b", 5);
    if (idx < sizeof(LVLS)) robimon::hal::display::set_brightness(LVLS[idx]);
  }

  robimon::face::begin();
  // Stage D: touch drives expressions. Demo cycle off by default; the user
  // can re-enable via face::enable_demo_cycle(true) for visual checks.
  robimon::face::enable_demo_cycle(false);

  robimon::ui::gestures::begin();

  // Stage E/H: screen manager carousel — face → alarms → ha entity.
  // Settings is reached via long-press + PIN, NOT swipe.
  robimon::ui::screen_mgr::begin();
  robimon::ui::screen_mgr::add_screen(&robimon::screens::face_screen);
  robimon::ui::screen_mgr::add_screen(&robimon::screens::alarms_screen);
  robimon::ui::screen_mgr::add_screen(&robimon::screens::ha_entity_screen);

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

  // While an alarm is firing, taps dismiss and swipes snooze (per spec).
  // This routing happens BEFORE the regular screen_mgr dispatch so we can
  // intercept gestures regardless of which screen is showing.
  const bool alarm_firing = robimon::services::alarm_mgr::any_firing();

  switch (event) {
    case robimon::ui::gestures::Event::TAP: {
      const auto pt = robimon::ui::gestures::last_event_point();
      if (alarm_firing) {
        LOG_I(TAG, "alarm dismiss (tap)");
        robimon::services::alarm_mgr::dismiss_current();
      } else {
        robimon::ui::screen_mgr::on_tap(pt.x, pt.y);
      }
      break;
    }
    case robimon::ui::gestures::Event::LONG_PRESS:
      // Long-press opens the PIN pad if no modal is already up.
      if (alarm_firing) {
        // Treat long-press as dismiss too (more forgiving than only tap).
        robimon::services::alarm_mgr::dismiss_current();
      } else if (robimon::ui::screen_mgr::modal_depth() == 0) {
        LOG_I(TAG, "long-press -> PIN pad");
        robimon::ui::screen_mgr::push_modal(&robimon::screens::pin_pad_screen);
      }
      break;
    case robimon::ui::gestures::Event::SWIPE_LEFT:
    case robimon::ui::gestures::Event::SWIPE_RIGHT:
      if (alarm_firing) {
        LOG_I(TAG, "alarm snooze (swipe)");
        robimon::services::alarm_mgr::snooze_current(/*minutes=*/9);
      } else if (!robimon::face::menu_is_open()) {
        if (event == robimon::ui::gestures::Event::SWIPE_LEFT)
          robimon::ui::screen_mgr::next();
        else
          robimon::ui::screen_mgr::prev();
      }
      break;
    default:
      break;
  }

  robimon::services::wifi_mgr::update(millis());
  robimon::services::time_svc::update(millis());
  robimon::services::alarm_mgr::update(millis());
  robimon::services::ha_client::update(millis());
  robimon::services::serial_console::update(millis());

  // Watch for alarm firing transitions so the face overlay + alarm sound
  // match the alarm_mgr state. Doing this in main keeps alarm_mgr free of
  // UI/HAL deps.
  const int firing_idx = robimon::services::alarm_mgr::current_firing_idx();
  if (firing_idx != s_alarm_watcher.last_idx) {
    s_alarm_watcher.last_idx = firing_idx;
    if (firing_idx >= 0) {
      const auto* a = robimon::services::alarm_mgr::current_firing();
      if (a) {
        robimon::face::set_expression((robimon::face::Expression)a->expression, 200);
        robimon::face::set_label(a->label[0] ? a->label : "alarm");
      }
      // Two-tone alarm chime on a background task; loops until dismissed.
      robimon::hal::audio::start_alarm();
    } else {
      // Returning to normal: stop sound, drop label, tween back to neutral.
      robimon::hal::audio::stop_alarm();
      robimon::face::clear_label();
      robimon::face::set_expression(robimon::face::Expression::NEUTRAL, 350);
    }
  }

  robimon::ui::screen_mgr::update(millis());
  robimon::stats::note_frame();
  robimon::stats::tick();
  delay(2);
}
