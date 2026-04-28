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
#include <esp_task_wdt.h>

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
#include "services/setup_portal.h"
#include "services/voice_client.h"
#include "services/tutorial.h"
#include "services/proximity_monitor.h"
#include "services/companion_client.h"
#include "services/playback_server.h"
#include "services/conversation_session.h"
#include "screens/setup_screen.h"
#include "app/log.h"
#include "app/stats.h"

namespace {
struct AlarmFireWatcher { int last_idx = -2; } s_alarm_watcher;
bool     s_setup_mode = false;
uint32_t s_last_activity_ms = 0;

// Lightweight motion watcher — uses the IMU accel polling that stats already
// does. We compare each sample's magnitude to a running mean; a sudden
// deviation (kid bumps the desk, picks Robimon up) counts as user activity
// and wakes the screen out of dim/blank. Lower-power than the QMI8658's
// interrupt-driven wake-on-motion, but doesn't require touching the
// IDF wake source plumbing.
struct MotionWatch {
  float    mean_g  = 1.0f;     // gravity baseline
  uint32_t last_check_ms = 0;
  uint32_t last_react_ms = 0;
} s_motion;
constexpr float    MOTION_THRESHOLD_G  = 0.20f;
constexpr uint32_t MOTION_CHECK_MS     = 100;
constexpr uint32_t PICKUP_REACT_COOLDOWN_MS = 5000;   // throttle "whoa!" to ~once per 5s

// Boot-greeting state. Shown once per power-on, after NTP sync (or after
// a short grace period if SNTP doesn't come up).
bool     s_greeted = false;
uint32_t s_greet_grace_until_ms = 0;

// Screen-time awareness. Tracks one continuous-active "session" — kid
// picks up Robimon, plays for a while, eventually gets a gentle "take a
// break?" prompt. Session ends after IDLE_RESET_MS of no activity; next
// activity starts a new session. Daily limits / bedtime are deferred —
// this is the minimum that pays off for parent trust.
uint32_t s_session_start_ms      = 0;
bool     s_break_prompted        = false;
constexpr uint32_t SESSION_PROMPT_MS    = 30UL * 60UL * 1000UL;   // 30 min
constexpr uint32_t SESSION_IDLE_RESET_MS = 5UL * 60UL * 1000UL;    //  5 min
}

namespace {
constexpr const char* TAG = "main";
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  LOG_I(TAG, "Robimon %s booting", ROBIMON_VERSION);

  // Bump task watchdog timeout to 45 s. The default (5 s on Arduino-ESP32 3.x)
  // panics during a slow voice round-trip (Whisper + LLM + Piper can take
  // 10-20 s). Subscribed tasks must call esp_task_wdt_reset() before the
  // timeout; the main loop's per-iteration reset below covers itself, and
  // voice_task / alarm_task subscribe themselves and reset internally.
  const esp_task_wdt_config_t wdt_cfg = {
      .timeout_ms     = 45000,
      .idle_core_mask = 0,
      .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdt_cfg);
  // Subscribe the loop task. ESP_ERR_INVALID_ARG = already subscribed (depends
  // on Arduino-ESP32 build options); harmless either way.
  esp_task_wdt_add(NULL);

  Wire.begin(robimon::board::I2C_SDA, robimon::board::I2C_SCL, robimon::board::I2C_FREQ_HZ);
  LOG_I(TAG, "I2C up: SDA=%d SCL=%d @ %lu Hz",
        robimon::board::I2C_SDA, robimon::board::I2C_SCL, (unsigned long)robimon::board::I2C_FREQ_HZ);

  // Order matters here — see the file header comment.
  if (!robimon::hal::ioexp::begin())   LOG_W(TAG, "ioexp not ready");
  if (!robimon::hal::power::begin())   LOG_W(TAG, "PMIC not ready");

  if (!robimon::hal::display::begin()) {
    LOG_E(TAG, "display init failed; restarting");
    delay(1000);
    ESP.restart();
  }

  if (!robimon::hal::touch::begin())   LOG_W(TAG, "touch not ready");
  if (!robimon::hal::imu::begin())     LOG_W(TAG, "IMU not ready");
  if (!robimon::hal::audio::begin())   LOG_W(TAG, "audio not ready");
  if (!robimon::services::config::begin()) LOG_W(TAG, "config store not ready");

  // Setup-mode routing. Two signals:
  //   force_setup  — one-shot flag set by the about-screen "rerun setup"
  //                  button. Takes priority and is cleared on this boot.
  //   configured   — long-lived flag set when setup completes successfully.
  //
  // Without force_setup: enter setup mode iff configured is 0. The grace
  // path also marks configured=1 on the first boot of this firmware for
  // users who already had wifi creds via the serial console.
  const bool force_setup = robimon::services::config::get_uint("force_setup", 0) != 0;
  if (force_setup) {
    LOG_I(TAG, "force_setup flag — entering setup mode");
    robimon::services::config::set_uint("force_setup", 0);
    s_setup_mode = true;
  } else {
    s_setup_mode = robimon::services::config::get_uint("configured", 0) == 0;
    if (s_setup_mode) {
      char existing_ssid[33] = {0};
      if (robimon::services::config::get_string("wifi_ssid", existing_ssid, sizeof(existing_ssid)) > 0) {
        LOG_I(TAG, "existing wifi creds present — marking configured");
        robimon::services::config::set_uint("configured", 1);
        s_setup_mode = false;
      }
    }
  }

  if (s_setup_mode) {
    LOG_I(TAG, "no config in NVS — entering setup mode");
    robimon::services::setup_portal::begin();
    robimon::services::serial_console::begin();   // still useful in setup mode

    robimon::face::begin();
    robimon::face::enable_demo_cycle(false);
    robimon::ui::screen_mgr::begin();
    robimon::ui::screen_mgr::add_screen(&robimon::screens::setup_screen);
    robimon::stats::begin();
    LOG_I(TAG, "setup mode ready");
    return;
  }

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

  // Voice client (Stage J): double-tap on the face triggers it.
  robimon::services::voice::begin();

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

  // Start the idle timer at "now" so we don't immediately dim before the
  // user has a chance to interact.
  s_last_activity_ms = millis();

  // First-boot tutorial — runs once per device unless replay()'d. The
  // tutorial is purely observational: it watches the existing event
  // dispatch and advances its caption when the user does the right
  // gesture.
  robimon::services::tutorial::begin();

  // Proximity feature stack (bot-to-bot conversation):
  //   conversation_session  — local session bookkeeping + safety timeouts
  //   companion_client      — outbound HTTP worker (proximity + playback events)
  //   playback_server       — inbound HTTP for /play, /stop, /status
  //   proximity_monitor     — BLE RSSI state machine
  // The proximity callback runs on the proximity task (core 0); each branch
  // queues to the companion HTTP worker (also core 0) — no blocking on the
  // main UI loop.
  robimon::services::conversation_session::begin();
  robimon::services::companion_client::begin();
  if (!robimon::services::playback_server::begin()) {
    LOG_W(TAG, "playback server failed to start (PSRAM low?)");
  }
  robimon::services::proximity::begin(
      [](robimon::services::proximity::State new_state, int8_t rssi) {
        using S = robimon::services::proximity::State;
        // Track whether we've told the server "detected" so we can match
        // it with a "lost". Without this, the server's registry shows the
        // bot as in-proximity for the full PROXIMITY_HOLD_SECONDS window
        // even after the bot already left.
        static bool s_detected_sent = false;
        switch (new_state) {
          case S::IN_PROXIMITY:
            robimon::face::set_expression(robimon::face::Expression::EXCITED, 250);
            robimon::ui::screen_mgr::set_caption("found a friend!", 4000);
            robimon::services::companion_client::send_proximity_detected(rssi);
            s_detected_sent = true;
            // Wake the screen if it was dimmed/blanked.
            s_last_activity_ms = millis();
            break;
          case S::OUT_OF_RANGE:
            if (s_detected_sent) {
              robimon::services::companion_client::send_proximity_lost();
              s_detected_sent = false;
            }
            if (robimon::services::conversation_session::is_active()) {
              robimon::services::conversation_session::end_session("proximity_lost");
            }
            break;
          case S::APPROACHING:
          case S::LEAVING:
          case S::UNPAIRED:
            // No companion-app event for transitional or idle states.
            break;
        }
        LOG_I(TAG, "prox cb: %s @ %d dBm",
              robimon::services::proximity::state_str(new_state), (int)rssi);
      });

  // Boot greeting will fire once SNTP syncs (so we can pick a time-of-day
  // greeting) or after a 6 s grace window if it doesn't. Skipped if the
  // tutorial is still running — that takes precedence.
  s_greet_grace_until_ms = millis() + 6000;

#if defined(ROBIMON_BOOT_TEST_TONE) && (ROBIMON_BOOT_TEST_TONE == 1)
  if (robimon::hal::audio::ok()) {
    LOG_I(TAG, "playing boot test tone");
    robimon::hal::audio::play_test_tone();
  }
#endif

  LOG_I(TAG, "setup done");
}

void loop() {
  if (s_setup_mode) {
    // Setup mode: only the portal + serial console + screen render.
    // Touch is ignored (the user is on their phone, not the device).
    robimon::services::setup_portal::update(millis());
    robimon::services::serial_console::update(millis());
    robimon::ui::screen_mgr::update(millis());
    robimon::stats::note_frame();
    robimon::stats::tick();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  const uint32_t now_ms = millis();

  // Read touch and feed it to the gesture detector. Events route through
  // the screen manager to the active screen (or to nav, in the case of
  // swipes).
  robimon::hal::touch::Point hw_pts[1];
  const int n = robimon::hal::touch::read(hw_pts, 1);
  robimon::ui::gestures::Point gp{ n > 0 ? hw_pts[0].x : (int16_t)0,
                                    n > 0 ? hw_pts[0].y : (int16_t)0 };
  const auto event = robimon::ui::gestures::update(n, gp);

  // Activity tracking for the auto-dim / blank logic. Any touch counts;
  // an active alarm or voice round-trip also counts (so the screen doesn't
  // dim mid-conversation). Every-iteration call is cheap because the
  // display HAL only writes the panel register on transitions.
  if (n > 0
      || robimon::services::alarm_mgr::any_firing()
      || robimon::services::voice::state() != robimon::services::voice::State::IDLE) {
    s_last_activity_ms = now_ms;
  }

  // Motion-as-activity: poll the IMU at ~10 Hz; large excursions from the
  // running gravity baseline count as a wake event. Cheap on the bus
  // (one I2C burst at 10 Hz) and doesn't require dedicated hardware INT
  // routing.
  if (now_ms - s_motion.last_check_ms >= MOTION_CHECK_MS) {
    s_motion.last_check_ms = now_ms;
    robimon::hal::imu::Sample s;
    if (robimon::hal::imu::read(s)) {
      const float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
      const float delta = mag > s_motion.mean_g ? mag - s_motion.mean_g
                                                 : s_motion.mean_g - mag;
      if (delta > MOTION_THRESHOLD_G) {
        s_last_activity_ms = now_ms;

        // Pickup react: brief "whoa!" caption + surprised face. Throttled
        // and gated so we don't fire mid-conversation, mid-alarm, during
        // the tutorial intro, or while a finger is on the screen (an
        // active swipe registers as motion too).
        const bool busy = robimon::services::voice::state() != robimon::services::voice::State::IDLE
                       || robimon::services::alarm_mgr::any_firing()
                       || robimon::services::tutorial::is_active()
                       || n > 0;
        if (!busy && (now_ms - s_motion.last_react_ms) > PICKUP_REACT_COOLDOWN_MS) {
          s_motion.last_react_ms = now_ms;
          robimon::face::set_expression(robimon::face::Expression::SURPRISED, 200);
          robimon::ui::screen_mgr::set_caption("whoa!", 1500);
        }
      }
      // Slow exponential mean — recovers gravity baseline after orientation
      // changes without making the threshold trip on slow re-orientation.
      s_motion.mean_g = s_motion.mean_g * 0.95f + mag * 0.05f;
    }
  }

  // Screen-time session tracking — see the comment on s_session_start_ms.
  // The session reset window matches the auto-dim timer so "screen blanked"
  // implicitly ends a session.
  const uint32_t idle_for = now_ms - s_last_activity_ms;
  if (idle_for >= SESSION_IDLE_RESET_MS) {
    s_session_start_ms = 0;
    s_break_prompted   = false;
  } else if (s_session_start_ms == 0) {
    s_session_start_ms = now_ms;
  }
  if (!s_break_prompted && s_session_start_ms != 0
      && (now_ms - s_session_start_ms) >= SESSION_PROMPT_MS) {
    s_break_prompted = true;
    robimon::face::set_expression(robimon::face::Expression::HAPPY, 250);
    robimon::ui::screen_mgr::set_caption("take a break?", 5000);
    LOG_I(TAG, "30-min session reached — break prompt shown");
  }

  // Boot greeting — fires once per power-on, after SNTP syncs or after a
  // grace window. Suppressed while the tutorial is running.
  if (!s_greeted && !robimon::services::tutorial::is_active()) {
    const bool synced = robimon::services::time_svc::is_synced();
    if (synced || now_ms >= s_greet_grace_until_ms) {
      s_greeted = true;
      const char* msg = "hi!";
      if (synced) {
        struct tm lt;
        if (robimon::services::time_svc::get_local_time(&lt)) {
          const int h = lt.tm_hour;
          if      (h >=  5 && h < 12) msg = "good morning!";
          else if (h >= 12 && h < 17) msg = "afternoon!";
          else if (h >= 17 && h < 21) msg = "evening!";
          else                         msg = "shouldn't you be in bed?";
        }
      }
      robimon::face::set_expression(robimon::face::Expression::HAPPY, 250);
      robimon::ui::screen_mgr::set_caption(msg, 3000);
    }
  }

  robimon::hal::display::apply_idle_brightness(now_ms - s_last_activity_ms);

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
        // Briefly glance at the touch — even when the tap goes to a non-
        // face screen the eyes track, which makes Robimon feel attentive
        // beyond the face screen alone.
        robimon::face::glance_at(pt.x, pt.y);
        robimon::ui::screen_mgr::on_tap(pt.x, pt.y);
      }
      break;
    }
    case robimon::ui::gestures::Event::DOUBLE_TAP: {
      // Voice mode is only meaningful from the face screen; on other
      // screens we ignore double-tap and let the prior TAP stand.
      const auto* cur = robimon::ui::screen_mgr::current();
      if (cur && strcmp(cur->name(), "face") == 0) {
        // The first tap already opened the radial menu — close it back up
        // so the user lands directly in voice mode.
        robimon::face::dismiss_menu();
        LOG_I(TAG, "double-tap -> voice");
        robimon::services::voice::start();
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
        robimon::services::tutorial::on_long_press();
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
        robimon::services::tutorial::on_swipe();
      }
      break;
    default:
      break;
  }

  robimon::services::wifi_mgr::update(millis());
  robimon::services::time_svc::update(millis());
  robimon::services::alarm_mgr::update(millis());
  robimon::services::ha_client::update(millis());
  robimon::services::voice::update(millis());
  robimon::services::serial_console::update(millis());
  robimon::services::tutorial::update(millis());
  robimon::services::conversation_session::update(millis());

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
  esp_task_wdt_reset();   // kick the dog every iteration
  // Yield to FreeRTOS rather than busy-spinning so equal-priority tasks get
  // their share. The face renderer caps frames at 30 FPS internally — this
  // delay just relaxes the polling rate of touch + state machines.
  vTaskDelay(pdMS_TO_TICKS(2));
}
